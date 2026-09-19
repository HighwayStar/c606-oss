/*
 * Ride recorder: a standard FIT activity file.
 *
 * Message order follows the FIT activity file layout: file_id, file_creator,
 * device_info (this unit + the paired ANT+ sensors), event timer start,
 * then one record per second while the timer runs (timer stop/start events
 * around pauses), a lap message when a lap completes, and at the end a last
 * lap, the session, the activity and the file CRC. Values that are not
 * available (no fix, no sensor) are written as the base type's "invalid"
 * value, as the protocol prescribes.
 *
 * Every data message is a packed struct whose members are in the order of the
 * matching definition table; fit_write() checks the sizes agree.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "sdcard.h"
#include "tracklog.h"
#include "fit.h"
#include "utc.h"
#include "gps.h"
#include "ant.h"
#include "stats.h"
#include "trip.h"
#include "ride.h"
#include "config.h"
#include "health.h"

static const char *TAG = "track";
#define TRACK_DIR TRACKLOG_DIR

/* identity written to file_id / device_info. `development` is what the
 * profile reserves for unregistered products. */
#define FIT_MANUFACTURER   255      /* development */
#define FIT_PRODUCT        1
#define FIT_PRODUCT_NAME   "c606-oss"
#define FIT_SW_VERSION     100      /* 1.00 */
#define FIT_HW_VERSION     1

#define PENDING_TIMEOUT_MS 20000    /* start without a clock after this long */
#define SYNC_EVERY_RECORDS 10       /* fsync interval: a power loss costs ~10 s */
#define ASCENT_HYST_M      5.0f     /* altitude change below this is noise */
#define GPS_FRESH_MS       3000

/* ---- profile enums ---------------------------------------------------- */
#define FILE_ACTIVITY          4
#define EVENT_TIMER            0
#define EVENT_LAP              9
#define EVENT_SESSION          8
#define EVENT_ACTIVITY         26
#define EVT_START              0
#define EVT_STOP               1
#define EVT_STOP_ALL           4
#define LAP_TRIG_MANUAL        0
#define LAP_TRIG_DISTANCE      2
#define LAP_TRIG_SESSION_END   7
#define SESSION_TRIG_ACT_END   0
#define ACTIVITY_MANUAL        0
#define SPORT_CYCLING          2
#define SUB_SPORT_GENERIC      0
#define SOURCE_ANTPLUS         1
#define SOURCE_LOCAL           5
#define DEVICE_INDEX_CREATOR   0

/* ---- message tables + packed payloads --------------------------------- */
#define PACKED __attribute__((packed))

static const fit_field_t k_file_id_f[] = {
    { 0, 1, FIT_ENUM }, { 1, 2, FIT_UINT16 }, { 2, 2, FIT_UINT16 }, { 3, 4, FIT_UINT32Z },
    { 4, 4, FIT_UINT32 }, { 8, 16, FIT_STRING },
};
static const fit_mesg_t k_file_id = { 0, 6, k_file_id_f };
typedef struct PACKED {
    uint8_t type; uint16_t manufacturer, product; uint32_t serial, time_created; char product_name[16];
} file_id_t;

static const fit_field_t k_file_creator_f[] = { { 0, 2, FIT_UINT16 }, { 1, 1, FIT_UINT8 } };
static const fit_mesg_t k_file_creator = { 49, 2, k_file_creator_f };
typedef struct PACKED { uint16_t sw_version; uint8_t hw_version; } file_creator_t;

static const fit_field_t k_device_info_f[] = {
    { 253, 4, FIT_UINT32 }, { 0, 1, FIT_UINT8 }, { 1, 1, FIT_UINT8 }, { 2, 2, FIT_UINT16 },
    { 4, 2, FIT_UINT16 }, { 3, 4, FIT_UINT32Z }, { 5, 2, FIT_UINT16 }, { 21, 2, FIT_UINT16Z },
    { 20, 1, FIT_UINT8Z }, { 25, 1, FIT_ENUM }, { 27, 16, FIT_STRING },
};
static const fit_mesg_t k_device_info = { 23, 11, k_device_info_f };
typedef struct PACKED {
    uint32_t timestamp; uint8_t device_index, device_type; uint16_t manufacturer, product;
    uint32_t serial; uint16_t sw_version, ant_number; uint8_t ant_trans_type, source_type;
    char product_name[16];
} device_info_t;

static const fit_field_t k_event_f[] = {
    { 253, 4, FIT_UINT32 }, { 0, 1, FIT_ENUM }, { 1, 1, FIT_ENUM }, { 4, 1, FIT_UINT8 },
};
static const fit_mesg_t k_event = { 21, 4, k_event_f };
typedef struct PACKED { uint32_t timestamp; uint8_t event, event_type, event_group; } event_t;

static const fit_field_t k_record_f[] = {
    { 253, 4, FIT_UINT32 }, { 0, 4, FIT_SINT32 }, { 1, 4, FIT_SINT32 }, { 5, 4, FIT_UINT32 },
    { 2, 2, FIT_UINT16 }, { 6, 2, FIT_UINT16 }, { 7, 2, FIT_UINT16 }, { 3, 1, FIT_UINT8 },
    { 4, 1, FIT_UINT8 }, { 13, 1, FIT_SINT8 },
};
static const fit_mesg_t k_record = { 20, 10, k_record_f };
typedef struct PACKED {
    uint32_t timestamp; int32_t lat, lon; uint32_t distance; uint16_t altitude, speed, power;
    uint8_t heart_rate, cadence; int8_t temperature;
} record_t;

/* lap and session share the summary block; only the field numbers differ */
typedef struct PACKED {
    uint32_t timestamp; uint16_t message_index; uint8_t event, event_type; uint32_t start_time;
    int32_t start_lat, start_lon;
} summary_head_t;
typedef struct PACKED {
    uint32_t elapsed_ms, timer_ms, distance; uint16_t avg_speed, max_speed;
    uint8_t avg_hr, max_hr, avg_cad, max_cad; uint16_t avg_power, max_power, ascent, descent;
    int8_t avg_temp, max_temp; uint16_t avg_alt, max_alt, min_alt;
    uint16_t calories; uint32_t hr_zone_ms[HR_ZONES + 1], pwr_zone_ms[PWR_ZONES + 1];
} summary_body_t;

static const fit_field_t k_lap_f[] = {
    { 253, 4, FIT_UINT32 }, { 254, 2, FIT_UINT16 }, { 0, 1, FIT_ENUM }, { 1, 1, FIT_ENUM },
    { 2, 4, FIT_UINT32 }, { 3, 4, FIT_SINT32 }, { 4, 4, FIT_SINT32 },
    { 5, 4, FIT_SINT32 }, { 6, 4, FIT_SINT32 }, { 24, 1, FIT_ENUM }, { 25, 1, FIT_ENUM }, { 39, 1, FIT_ENUM },
    { 7, 4, FIT_UINT32 }, { 8, 4, FIT_UINT32 }, { 9, 4, FIT_UINT32 }, { 13, 2, FIT_UINT16 }, { 14, 2, FIT_UINT16 },
    { 15, 1, FIT_UINT8 }, { 16, 1, FIT_UINT8 }, { 17, 1, FIT_UINT8 }, { 18, 1, FIT_UINT8 },
    { 19, 2, FIT_UINT16 }, { 20, 2, FIT_UINT16 }, { 21, 2, FIT_UINT16 }, { 22, 2, FIT_UINT16 },
    { 50, 1, FIT_SINT8 }, { 51, 1, FIT_SINT8 }, { 42, 2, FIT_UINT16 }, { 43, 2, FIT_UINT16 }, { 62, 2, FIT_UINT16 },
    { 11, 2, FIT_UINT16 }, { 57, 4 * (HR_ZONES + 1), FIT_UINT32 }, { 60, 4 * (PWR_ZONES + 1), FIT_UINT32 },
};
static const fit_mesg_t k_lap = { 19, 33, k_lap_f };
typedef struct PACKED {
    summary_head_t h; int32_t end_lat, end_lon; uint8_t trigger, sport, sub_sport; summary_body_t b;
} lap_t;

static const fit_field_t k_session_f[] = {
    { 253, 4, FIT_UINT32 }, { 254, 2, FIT_UINT16 }, { 0, 1, FIT_ENUM }, { 1, 1, FIT_ENUM },
    { 2, 4, FIT_UINT32 }, { 3, 4, FIT_SINT32 }, { 4, 4, FIT_SINT32 },
    { 5, 1, FIT_ENUM }, { 6, 1, FIT_ENUM }, { 25, 2, FIT_UINT16 }, { 26, 2, FIT_UINT16 }, { 28, 1, FIT_ENUM },
    { 7, 4, FIT_UINT32 }, { 8, 4, FIT_UINT32 }, { 9, 4, FIT_UINT32 }, { 14, 2, FIT_UINT16 }, { 15, 2, FIT_UINT16 },
    { 16, 1, FIT_UINT8 }, { 17, 1, FIT_UINT8 }, { 18, 1, FIT_UINT8 }, { 19, 1, FIT_UINT8 },
    { 20, 2, FIT_UINT16 }, { 21, 2, FIT_UINT16 }, { 22, 2, FIT_UINT16 }, { 23, 2, FIT_UINT16 },
    { 57, 1, FIT_SINT8 }, { 58, 1, FIT_SINT8 }, { 49, 2, FIT_UINT16 }, { 50, 2, FIT_UINT16 }, { 71, 2, FIT_UINT16 },
    { 11, 2, FIT_UINT16 }, { 65, 4 * (HR_ZONES + 1), FIT_UINT32 }, { 68, 4 * (PWR_ZONES + 1), FIT_UINT32 },
};
static const fit_mesg_t k_session = { 18, 33, k_session_f };
typedef struct PACKED {
    summary_head_t h; uint8_t sport, sub_sport; uint16_t first_lap_index, num_laps; uint8_t trigger; summary_body_t b;
} session_t;

static const fit_field_t k_activity_f[] = {
    { 253, 4, FIT_UINT32 }, { 0, 4, FIT_UINT32 }, { 1, 2, FIT_UINT16 }, { 2, 1, FIT_ENUM },
    { 3, 1, FIT_ENUM }, { 4, 1, FIT_ENUM }, { 5, 4, FIT_UINT32 },
};
static const fit_mesg_t k_activity = { 34, 7, k_activity_f };
typedef struct PACKED {
    uint32_t timestamp, timer_ms; uint16_t num_sessions; uint8_t type, event, event_type; uint32_t local_timestamp;
} activity_t;

/* rider profile and the zone definitions the time-in-zone arrays refer to */
static const fit_field_t k_user_profile_f[] = {
    { 1, 1, FIT_ENUM }, { 2, 1, FIT_UINT8 }, { 3, 1, FIT_UINT8 }, { 4, 2, FIT_UINT16 }, { 11, 1, FIT_UINT8 },
};
static const fit_mesg_t k_user_profile = { 3, 5, k_user_profile_f };
typedef struct PACKED { uint8_t gender, age, height; uint16_t weight; uint8_t default_max_hr; } user_profile_t;

static const fit_field_t k_zones_target_f[] = {
    { 1, 1, FIT_UINT8 }, { 2, 1, FIT_UINT8 }, { 3, 2, FIT_UINT16 }, { 5, 1, FIT_ENUM }, { 7, 1, FIT_ENUM },
};
static const fit_mesg_t k_zones_target = { 7, 5, k_zones_target_f };
typedef struct PACKED { uint8_t max_hr, threshold_hr; uint16_t ftp; uint8_t hr_calc_type, pwr_calc_type; } zones_target_t;
#define HR_CALC_PCT_MAX   1
#define HR_CALC_PCT_LTHR  3
#define PWR_CALC_PCT_FTP  1
#define GENDER_FEMALE     0
#define GENDER_MALE       1

/* local message numbers: records keep theirs, everything else shares one
 * (its definition is re-sent whenever the message type changes) */
#define LOCAL_RECORD 0
#define LOCAL_OTHER  1

/* ---- per-lap / per-session accumulators -------------------------------- */
typedef struct {
    uint32_t start_ts;         /* FIT timestamp */
    uint32_t start_timer_ms;   /* stats_session_ms() at the start */
    float start_dist_m;
    int32_t start_lat, start_lon;
    uint32_t n;                /* records */
    uint32_t hr_sum, hr_n, hr_max;
    uint32_t cad_sum, cad_n, cad_max;
    uint32_t pwr_sum, pwr_n, pwr_max;
    float max_speed_ms;
    uint32_t speed_n;
    int32_t temp_sum, temp_n, temp_max;
    float alt_sum, alt_min, alt_max, alt_ref;
    uint32_t alt_n;
    float ascent, descent;
    float start_kcal;                    /* health.c totals at the start */
    uint32_t start_hr_zone_ms[HR_ZONES + 1], start_pwr_zone_ms[PWR_ZONES + 1];
} acc_t;

static SemaphoreHandle_t s_lock;
static fit_writer_t s_w;
static char s_name[64];
static bool s_pending;             /* start requested, waiting for the clock */
static bool s_paused;
static uint32_t s_start_ms;        /* ESP timer at ride start */
static uint32_t s_pending_since_ms;
static uint32_t s_base_ts;         /* FIT timestamp of the ride start */
static uint32_t s_last_ts;         /* timestamp of the last record */
static uint32_t s_records, s_unsynced;
static uint32_t s_laps_written;
static int32_t s_last_lat = FIT_INV_S32, s_last_lon = FIT_INV_S32;
static acc_t s_lap, s_session;

static uint32_t now_ms(void) { return esp_timer_get_time() / 1000; }
static uint32_t ts_now(void) { return s_base_ts + (now_ms() - s_start_ms) / 1000; }
static int32_t semicircles(double deg) { return (int32_t)(deg * (2147483648.0 / 180.0)); }
static uint16_t alt_u16(float m) { return m > -500.0f && m < 12000.0f ? (uint16_t)((m + 500.0f) * 5.0f + 0.5f) : FIT_INV_U16; }
static uint16_t speed_u16(float ms) { return ms < 65.0f ? (uint16_t)(ms * 1000.0f + 0.5f) : FIT_INV_U16; }
static uint8_t clamp_u8(uint32_t v) { return v < 0xFF ? v : 0xFE; }
static uint16_t clamp_u16(uint32_t v) { return v < 0xFFFF ? v : 0xFFFE; }

static uint32_t serial_number(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint32_t s = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    return s ? s : 1;   /* uint32z: 0 means "invalid" */
}

static void acc_start(acc_t *a, uint32_t ts)
{
    memset(a, 0, sizeof *a);
    a->start_ts = ts;
    a->start_timer_ms = stats_session_ms();
    a->start_dist_m = trip_distance_m();
    a->start_lat = s_last_lat;
    a->start_lon = s_last_lon;
    a->temp_max = INT32_MIN;
    a->start_kcal = health_kcal();
    for (int z = 0; z <= HR_ZONES; z++) a->start_hr_zone_ms[z] = health_hr_zone_ms(z);
    for (int z = 0; z <= PWR_ZONES; z++) a->start_pwr_zone_ms[z] = health_pwr_zone_ms(z);
}

static void acc_add(acc_t *a, const record_t *r, float speed_ms, float alt_m, bool alt_valid)
{
    a->n++;
    if (a->start_lat == FIT_INV_S32 && r->lat != FIT_INV_S32) {
        a->start_lat = r->lat;
        a->start_lon = r->lon;
    }
    if (r->heart_rate != FIT_INV_U8 && r->heart_rate) {
        a->hr_sum += r->heart_rate; a->hr_n++;
        if (r->heart_rate > a->hr_max) a->hr_max = r->heart_rate;
    }
    if (r->cadence != FIT_INV_U8 && r->cadence) {
        a->cad_sum += r->cadence; a->cad_n++;
        if (r->cadence > a->cad_max) a->cad_max = r->cadence;
    }
    if (r->power != FIT_INV_U16) {
        a->pwr_sum += r->power; a->pwr_n++;
        if (r->power > a->pwr_max) a->pwr_max = r->power;
    }
    if (r->speed != FIT_INV_U16) {
        a->speed_n++;
        if (speed_ms > a->max_speed_ms) a->max_speed_ms = speed_ms;
    }
    if (r->temperature != FIT_INV_S8) {
        a->temp_sum += r->temperature; a->temp_n++;
        if (r->temperature > a->temp_max) a->temp_max = r->temperature;
    }
    if (alt_valid) {
        if (a->alt_n == 0) {
            a->alt_min = a->alt_max = a->alt_ref = alt_m;
        } else {
            if (alt_m < a->alt_min) a->alt_min = alt_m;
            if (alt_m > a->alt_max) a->alt_max = alt_m;
            /* climb/descent with hysteresis against GPS altitude noise */
            float d = alt_m - a->alt_ref;
            if (d >= ASCENT_HYST_M) { a->ascent += d; a->alt_ref = alt_m; }
            else if (d <= -ASCENT_HYST_M) { a->descent -= d; a->alt_ref = alt_m; }
        }
        a->alt_sum += alt_m;
        a->alt_n++;
    }
}

static void acc_fill(const acc_t *a, summary_head_t *h, summary_body_t *b, uint32_t ts,
                     uint32_t timer_ms, float dist_m, uint16_t index, uint8_t event)
{
    h->timestamp = ts;
    h->message_index = index;
    h->event = event;
    h->event_type = EVT_STOP;
    h->start_time = a->start_ts;
    h->start_lat = a->start_lat;
    h->start_lon = a->start_lon;

    b->elapsed_ms = (ts - a->start_ts) * 1000;
    b->timer_ms = timer_ms;
    b->distance = (uint32_t)(dist_m * 100.0f + 0.5f);
    b->avg_speed = timer_ms >= 1000 ? speed_u16(dist_m / (timer_ms / 1000.0f)) : FIT_INV_U16;
    b->max_speed = a->speed_n ? speed_u16(a->max_speed_ms) : FIT_INV_U16;
    b->avg_hr = a->hr_n ? clamp_u8(a->hr_sum / a->hr_n) : FIT_INV_U8;
    b->max_hr = a->hr_n ? clamp_u8(a->hr_max) : FIT_INV_U8;
    b->avg_cad = a->cad_n ? clamp_u8(a->cad_sum / a->cad_n) : FIT_INV_U8;
    b->max_cad = a->cad_n ? clamp_u8(a->cad_max) : FIT_INV_U8;
    b->avg_power = a->pwr_n ? clamp_u16(a->pwr_sum / a->pwr_n) : FIT_INV_U16;
    b->max_power = a->pwr_n ? clamp_u16(a->pwr_max) : FIT_INV_U16;
    b->ascent = a->alt_n ? clamp_u16((uint32_t)(a->ascent + 0.5f)) : FIT_INV_U16;
    b->descent = a->alt_n ? clamp_u16((uint32_t)(a->descent + 0.5f)) : FIT_INV_U16;
    b->avg_temp = a->temp_n ? (int8_t)(a->temp_sum / (int32_t)a->temp_n) : FIT_INV_S8;
    b->max_temp = a->temp_n ? (int8_t)a->temp_max : FIT_INV_S8;
    b->avg_alt = a->alt_n ? alt_u16(a->alt_sum / a->alt_n) : FIT_INV_U16;
    b->max_alt = a->alt_n ? alt_u16(a->alt_max) : FIT_INV_U16;
    b->min_alt = a->alt_n ? alt_u16(a->alt_min) : FIT_INV_U16;
    float kcal = health_kcal() - a->start_kcal;
    b->calories = kcal > 0 ? clamp_u16((uint32_t)(kcal + 0.5f)) : 0;
    for (int z = 0; z <= HR_ZONES; z++) {
        b->hr_zone_ms[z] = a->hr_n ? health_hr_zone_ms(z) - a->start_hr_zone_ms[z] : FIT_INV_U32;
    }
    for (int z = 0; z <= PWR_ZONES; z++) {
        b->pwr_zone_ms[z] = a->pwr_n && health_pwr_zones_available() ? health_pwr_zone_ms(z) - a->start_pwr_zone_ms[z] : FIT_INV_U32;
    }
}

/* ---- writers ------------------------------------------------------------- */
static esp_err_t write_event(uint32_t ts, uint8_t event_type)
{
    event_t e = { .timestamp = ts, .event = EVENT_TIMER, .event_type = event_type, .event_group = 0 };
    return fit_write(&s_w, LOCAL_OTHER, &k_event, &e, sizeof e);
}

static esp_err_t write_preamble(uint32_t ts)
{
    file_id_t id = {
        .type = FILE_ACTIVITY, .manufacturer = FIT_MANUFACTURER, .product = FIT_PRODUCT,
        .serial = serial_number(), .time_created = ts,
    };
    strncpy(id.product_name, FIT_PRODUCT_NAME, sizeof id.product_name - 1);
    esp_err_t err = fit_write(&s_w, LOCAL_OTHER, &k_file_id, &id, sizeof id);

    file_creator_t fc = { .sw_version = FIT_SW_VERSION, .hw_version = FIT_HW_VERSION };
    if (err == ESP_OK) err = fit_write(&s_w, LOCAL_OTHER, &k_file_creator, &fc, sizeof fc);

    device_info_t di = {
        .timestamp = ts, .device_index = DEVICE_INDEX_CREATOR, .device_type = FIT_INV_U8,
        .manufacturer = FIT_MANUFACTURER, .product = FIT_PRODUCT, .serial = serial_number(),
        .sw_version = FIT_SW_VERSION, .ant_number = FIT_INV_U16Z, .ant_trans_type = FIT_INV_U8Z,
        .source_type = SOURCE_LOCAL,
    };
    strncpy(di.product_name, FIT_PRODUCT_NAME, sizeof di.product_name - 1);
    if (err == ESP_OK) err = fit_write(&s_w, LOCAL_OTHER, &k_device_info, &di, sizeof di);

    const app_cfg_t *cfg = config_get();
    for (int i = 0; i < cfg->nsensors && err == ESP_OK; i++) {
        const cfg_sensor_t *sn = &cfg->sensors[i];
        device_info_t ds = {
            .timestamp = ts, .device_index = i + 1, .device_type = sn->dev_type,
            .manufacturer = FIT_INV_U16, .product = FIT_INV_U16, .serial = FIT_INV_U32Z,
            .sw_version = FIT_INV_U16, .ant_number = sn->dev_num, .ant_trans_type = sn->trans_type,
            .source_type = SOURCE_ANTPLUS,
        };
        err = fit_write(&s_w, LOCAL_OTHER, &k_device_info, &ds, sizeof ds);
    }
    int age = health_age();
    user_profile_t up = {
        .gender = cfg->sex ? GENDER_FEMALE : GENDER_MALE, .age = age ? age : FIT_INV_U8,
        .height = cfg->height_cm, .weight = cfg->weight_kg * 10, .default_max_hr = health_max_hr(),
    };
    if (err == ESP_OK) err = fit_write(&s_w, LOCAL_OTHER, &k_user_profile, &up, sizeof up);
    zones_target_t zt = {
        .max_hr = health_max_hr(), .threshold_hr = health_lthr(),
        .ftp = cfg->ftp_w ? cfg->ftp_w : FIT_INV_U16,
        .hr_calc_type = cfg->hr_zone_mode == HRZ_PCT_LTHR ? HR_CALC_PCT_LTHR : HR_CALC_PCT_MAX,
        .pwr_calc_type = PWR_CALC_PCT_FTP,
    };
    if (err == ESP_OK) err = fit_write(&s_w, LOCAL_OTHER, &k_zones_target, &zt, sizeof zt);
    if (err == ESP_OK) err = write_event(ts, EVT_START);
    return err;
}

static esp_err_t write_lap(uint32_t ts, uint8_t trigger, uint32_t timer_ms, float dist_m)
{
    lap_t l = { .end_lat = s_last_lat, .end_lon = s_last_lon, .trigger = trigger,
                .sport = SPORT_CYCLING, .sub_sport = SUB_SPORT_GENERIC };
    acc_fill(&s_lap, &l.h, &l.b, ts, timer_ms, dist_m, s_laps_written, EVENT_LAP);
    esp_err_t err = fit_write(&s_w, LOCAL_OTHER, &k_lap, &l, sizeof l);
    s_laps_written++;
    return err;
}

static esp_err_t write_closing(uint32_t ts)
{
    esp_err_t err = ESP_OK;
    if (!s_paused) err = write_event(ts, EVT_STOP_ALL);
    /* the open lap ends with the session */
    if (err == ESP_OK) err = write_lap(ts, LAP_TRIG_SESSION_END, trip_lap_time_ms(), trip_lap_distance_m());

    session_t s = { .sport = SPORT_CYCLING, .sub_sport = SUB_SPORT_GENERIC, .first_lap_index = 0,
                    .num_laps = s_laps_written, .trigger = SESSION_TRIG_ACT_END };
    acc_fill(&s_session, &s.h, &s.b, ts, stats_session_ms(), trip_distance_m(), 0, EVENT_SESSION);
    if (err == ESP_OK) err = fit_write(&s_w, LOCAL_OTHER, &k_session, &s, sizeof s);

    activity_t a = {
        .timestamp = ts, .timer_ms = stats_session_ms(), .num_sessions = 1, .type = ACTIVITY_MANUAL,
        .event = EVENT_ACTIVITY, .event_type = EVT_STOP, .local_timestamp = ts + config_get()->tz_min * 60,
    };
    if (err == ESP_OK) err = fit_write(&s_w, LOCAL_OTHER, &k_activity, &a, sizeof a);
    return err;
}

/* One record from the current sensor state. */
static void write_record(uint32_t ts)
{
    gps_fix_t g;
    ant_sensors_t a;
    stat_values_t t;
    gps_get(&g);
    ant_get(&a);
    stats_get(STAT_TEMP, &t);
    uint32_t now = now_ms();
    bool gps_ok = g.valid && now - g.last_rx_ms < GPS_FRESH_MS;
    bool wheel = ant_live(ANT_DEV_SPEED, ANT_DEV_SPD_CAD);
    float speed_ms = wheel ? a.speed_kmh / 3.6f : gps_ok ? g.speed_kmh / 3.6f : -1;

    record_t r = {
        .timestamp = ts,
        .lat = gps_ok ? semicircles(g.lat) : FIT_INV_S32,
        .lon = gps_ok ? semicircles(g.lon) : FIT_INV_S32,
        .distance = (uint32_t)(trip_distance_m() * 100.0f + 0.5f),
        .altitude = gps_ok ? alt_u16(g.alt_m) : FIT_INV_U16,
        .speed = speed_ms >= 0 ? speed_u16(speed_ms) : FIT_INV_U16,
        .power = ant_live(ANT_DEV_POWER, ANT_DEV_POWER) ? clamp_u16(a.power_w) : FIT_INV_U16,
        .heart_rate = ant_live(ANT_DEV_HR, ANT_DEV_HR) && a.hr_bpm ? a.hr_bpm : FIT_INV_U8,
        .cadence = ant_live(ANT_DEV_CADENCE, ANT_DEV_SPD_CAD) ? clamp_u8((uint32_t)(a.cadence_rpm + 0.5f)) : FIT_INV_U8,
        .temperature = t.valid && t.live ? (int8_t)lrintf(t.cur) : FIT_INV_S8,
    };
    if (fit_write(&s_w, LOCAL_RECORD, &k_record, &r, sizeof r) != ESP_OK) return;
    if (gps_ok) {
        s_last_lat = r.lat;
        s_last_lon = r.lon;
    }
    acc_add(&s_lap, &r, speed_ms, g.alt_m, gps_ok);
    acc_add(&s_session, &r, speed_ms, g.alt_m, gps_ok);
    s_records++;
    s_last_ts = ts;
    /* fatfs buffers in RAM; push to the card now and then */
    if (++s_unsynced >= SYNC_EVERY_RECORDS) {
        s_unsynced = 0;
        fit_sync(&s_w);
    }
}

/* ---- lifecycle ------------------------------------------------------------ */
static void open_file(uint32_t base_ts)
{
    s_base_ts = base_ts;
    mkdir(TRACK_DIR, 0775);
    time_t local = (time_t)(base_ts + FIT_EPOCH_UNIX) + config_get()->tz_min * 60;
    struct tm tm;
    gmtime_r(&local, &tm);
    snprintf(s_name, sizeof s_name, TRACK_DIR "/%04d%02d%02d-%02d%02d%02d.fit",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (fit_open(&s_w, s_name) != ESP_OK) {
        ESP_LOGW(TAG, "cannot create %s", s_name);
        return;
    }
    s_records = s_unsynced = 0;
    s_last_ts = 0;
    s_laps_written = trip_laps();   /* laps completed while the start was pending are lost */
    s_last_lat = s_last_lon = FIT_INV_S32;
    acc_start(&s_lap, base_ts);
    acc_start(&s_session, base_ts);
    if (write_preamble(base_ts) != ESP_OK) {
        ESP_LOGW(TAG, "preamble failed, dropping %s", s_name);
        fit_close(&s_w);
        return;
    }
    if (s_paused) write_event(ts_now(), EVT_STOP_ALL);
    fit_sync(&s_w);
    ESP_LOGI(TAG, "recording to %s", s_name);
}

esp_err_t tracklog_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!sdcard_info()->mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_w.f || s_pending) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_paused = false;
    s_start_ms = s_pending_since_ms = now_ms();
    s_pending = true;
    xSemaphoreGive(s_lock);
    tracklog_tick();   /* opens the file right away when the clock is known */
    return ESP_OK;
}

void tracklog_pause(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_paused) {
        s_paused = true;
        if (s_w.f) {
            write_event(ts_now(), EVT_STOP_ALL);
            fit_sync(&s_w);
        }
    }
    xSemaphoreGive(s_lock);
}

void tracklog_resume(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_paused) {
        s_paused = false;
        if (s_w.f) write_event(ts_now(), EVT_START);
    }
    xSemaphoreGive(s_lock);
}

void tracklog_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending = false;
    if (s_w.f) {
        uint32_t ts = ts_now();
        if (ts < s_last_ts) ts = s_last_ts;
        esp_err_t err = write_closing(ts);
        if (fit_close(&s_w) != ESP_OK || err != ESP_OK) {
            ESP_LOGW(TAG, "%s: error while closing, file may be incomplete", s_name);
        } else {
            ESP_LOGI(TAG, "closed %s, %lu records, %lu laps", s_name,
                     (unsigned long)s_records, (unsigned long)s_laps_written);
        }
    }
    xSemaphoreGive(s_lock);
}

void tracklog_flush(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_w.f) {
        fit_sync(&s_w);
        s_unsynced = 0;
    }
    xSemaphoreGive(s_lock);
}

void tracklog_tick(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending) {
        uint32_t unix_s, base = 0;
        uint32_t waited = now_ms() - s_start_ms;
        if (utc_now(&unix_s)) {
            base = FIT_TS(unix_s) - waited / 1000;
        } else if (now_ms() - s_pending_since_ms >= PENDING_TIMEOUT_MS) {
            /* no RTC (power gesture not done since flashing) and no GPS date:
             * record anyway, timestamped from 2000-01-01 */
            ESP_LOGW(TAG, "no clock; timestamps start at 2000-01-01");
            base = FIT_TS(utc_from_civil(2000, 1, 1, 0, 0, 0));
        }
        if (base) {
            s_pending = false;
            open_file(base);
        }
    }
    if (s_w.f && !s_paused && ride_recording()) {
        uint32_t ts = ts_now();
        if (ts != s_last_ts) write_record(ts);
        if (trip_laps() > s_laps_written) {
            write_lap(ts, trip_prev_lap_manual() ? LAP_TRIG_MANUAL : LAP_TRIG_DISTANCE,
                      trip_prev_lap_time_ms(), trip_prev_lap_distance_m());
            acc_start(&s_lap, ts);
            fit_sync(&s_w);
        }
    }
    xSemaphoreGive(s_lock);
}

bool tracklog_active(void) { return s_w.f != NULL || s_pending; }
uint32_t tracklog_points(void) { return s_records; }
const char *tracklog_filename(void) { return s_name; }
