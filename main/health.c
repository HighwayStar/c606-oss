/*
 * Rider profile, calories and training zones. See health.h.
 *
 * The calorie model follows the vendor firmware (RideDataProcess, see
 * docs/HARDWARE.md): once a second the best source decides the rate.
 *   power meter:  kcal/s = W / 990         (1 kJ of work ~ 1.01 kcal; the
 *                                           usual ~24 % metabolic efficiency)
 *   heart rate:   Keytel et al. 2005, kcal/min =
 *                   male   (-55.0969 + 0.6309 HR + 0.1988 kg + 0.2017 age) / 4.184
 *                   female (-20.4022 + 0.4472 HR - 0.1263 kg + 0.0740 age) / 4.184
 *                 (the vendor adds the weight term for women; the paper
 *                 subtracts it, which is what we do)
 *   speed only:   kcal/s = MET x kg / 3600 with the vendor's MET steps for
 *                 cycling: 4.2 below 16 km/h, 6.3 / 8.4 / 10.5 / 12.6 / 14.7
 *                 from 16 / 19.2 / 22.4 / 25.6 / 30.6 km/h, 16.8 from 32.
 */
#include <string.h>
#include <time.h>
#include "esp_timer.h"

#include "health.h"
#include "config.h"
#include "ant.h"
#include "gps.h"
#include "stats.h"
#include "utc.h"

#define GPS_FRESH_MS 3000

/* zone lower bounds in percent of the reference value */
static const uint8_t k_hr_pct_max[HR_ZONES]   = { 50, 60, 70, 80, 90 };
static const uint8_t k_hr_pct_lthr[HR_ZONES]  = { 68, 85, 90, 95, 100 };
static const uint8_t k_pwr_pct_ftp[PWR_ZONES] = { 0, 55, 75, 90, 105, 120, 150 };

static float s_kcal;
static const char *s_source = "--";
static uint32_t s_hr_zone_ms[HR_ZONES + 1];
static uint32_t s_pwr_zone_ms[PWR_ZONES + 1];
static int s_hr_zone = -1;               /* zone of the last live HR sample */
static uint32_t s_hr_zone_since_ms;      /* session time when it changed */

void health_init(void)
{
    health_reset();
}

void health_reset(void)
{
    s_kcal = 0;
    s_source = "--";
    memset(s_hr_zone_ms, 0, sizeof s_hr_zone_ms);
    memset(s_pwr_zone_ms, 0, sizeof s_pwr_zone_ms);
    s_hr_zone = -1;
    s_hr_zone_since_ms = 0;
}

/* ---- profile ------------------------------------------------------------ */

int health_age(void)
{
    uint32_t unix_s;
    uint16_t by = config_get()->birth_year;
    if (!by || !utc_now(&unix_s)) return 0;
    time_t t = (time_t)unix_s;
    struct tm tm;
    gmtime_r(&t, &tm);
    int age = tm.tm_year + 1900 - by;
    return age > 0 && age < 120 ? age : 0;
}

bool health_max_hr_auto(void) { return config_get()->max_hr == 0; }
bool health_lthr_auto(void)   { return config_get()->lthr == 0; }

int health_max_hr(void)
{
    const app_cfg_t *c = config_get();
    if (c->max_hr) return c->max_hr;
    int age = health_age();
    return age ? 220 - age : 180;
}

int health_lthr(void)
{
    const app_cfg_t *c = config_get();
    if (c->lthr) return c->lthr;
    return (health_max_hr() * 89 + 50) / 100;
}

float health_bmi(void)
{
    const app_cfg_t *c = config_get();
    if (!c->weight_kg || !c->height_cm) return 0;
    float h = c->height_cm / 100.0f;
    return c->weight_kg / (h * h);
}

const char *health_bmi_class(float bmi)
{
    if (bmi <= 0) return "--";
    if (bmi < 18.5f) return "underweight";
    if (bmi < 25.0f) return "normal";
    if (bmi < 30.0f) return "overweight";
    return "obese";
}

/* Mifflin-St Jeor resting energy expenditure */
int health_bmr_kcal(void)
{
    const app_cfg_t *c = config_get();
    int age = health_age();
    if (!c->weight_kg || !c->height_cm || !age) return 0;
    float bmr = 10.0f * c->weight_kg + 6.25f * c->height_cm - 5.0f * age + (c->sex ? -161.0f : 5.0f);
    return bmr > 0 ? (int)(bmr + 0.5f) : 0;
}

/* ---- zones -------------------------------------------------------------- */

int health_hr_zone_low(int z)
{
    if (z < 1 || z > HR_ZONES) return 0;
    if (config_get()->hr_zone_mode == HRZ_PCT_LTHR) return (health_lthr() * k_hr_pct_lthr[z - 1] + 50) / 100;
    return (health_max_hr() * k_hr_pct_max[z - 1] + 50) / 100;
}

int health_hr_zone(int bpm)
{
    int z = 0;
    for (int i = 1; i <= HR_ZONES; i++) {
        if (bpm >= health_hr_zone_low(i)) z = i;
    }
    return z;
}

bool health_pwr_zones_available(void) { return config_get()->ftp_w != 0; }

int health_pwr_zone_low(int z)
{
    if (z < 1 || z > PWR_ZONES) return 0;
    return (config_get()->ftp_w * k_pwr_pct_ftp[z - 1] + 50) / 100;
}

int health_pwr_zone(int watts)
{
    if (!health_pwr_zones_available()) return 0;
    int z = 0;
    for (int i = 1; i <= PWR_ZONES; i++) {
        if (watts >= health_pwr_zone_low(i)) z = i;
    }
    return z;
}

uint32_t health_hr_zone_ms(int z)  { return z >= 0 && z <= HR_ZONES ? s_hr_zone_ms[z] : 0; }
uint32_t health_pwr_zone_ms(int z) { return z >= 0 && z <= PWR_ZONES ? s_pwr_zone_ms[z] : 0; }

int health_hr_zone_current(void)
{
    stat_values_t v;
    stats_get(STAT_HR, &v);
    return v.valid && v.live ? health_hr_zone((int)v.cur) : -1;
}

int health_pwr_zone_current(void)
{
    stat_values_t v;
    stats_get(STAT_POWER, &v);
    return v.valid && v.live && health_pwr_zones_available() ? health_pwr_zone((int)v.cur) : -1;
}

uint32_t health_zone_since_ms(void)
{
    return s_hr_zone < 0 ? 0 : stats_session_ms() - s_hr_zone_since_ms;
}

/* ---- calories ----------------------------------------------------------- */

float health_kcal(void) { return s_kcal; }

float health_kcal_per_h(void)
{
    uint32_t ms = stats_session_ms();
    return ms >= 60000 ? s_kcal * 3600000.0f / ms : 0;
}

const char *health_kcal_source(void) { return s_source; }

static float met_for_speed(float kmh)
{
    if (kmh <= 0) return 0;
    if (kmh < 16.0f) return 4.2f;
    if (kmh < 19.2f) return 6.3f;
    if (kmh < 22.4f) return 8.4f;
    if (kmh < 25.6f) return 10.5f;
    if (kmh < 30.6f) return 12.6f;
    if (kmh < 32.0f) return 14.7f;
    return 16.8f;
}

/* kcal burnt in one second from the best available source */
static float kcal_second(void)
{
    const app_cfg_t *c = config_get();
    ant_sensors_t a;
    ant_get(&a);
    if (ant_live(ANT_DEV_POWER, ANT_DEV_POWER)) {
        s_source = "power";
        return a.power_w / 990.0f;
    }
    if (ant_live(ANT_DEV_HR, ANT_DEV_HR) && a.hr_bpm) {
        s_source = "HR";
        float age = health_age() ? health_age() : 35;
        float kg = c->weight_kg ? c->weight_kg : 75;
        float per_min = c->sex
            ? (-20.4022f + 0.4472f * a.hr_bpm - 0.1263f * kg + 0.0740f * age) / 4.184f
            : (-55.0969f + 0.6309f * a.hr_bpm + 0.1988f * kg + 0.2017f * age) / 4.184f;
        return per_min > 0 ? per_min / 60.0f : 0;
    }
    float kmh = -1;
    if (ant_live(ANT_DEV_SPEED, ANT_DEV_SPD_CAD)) {
        kmh = a.speed_kmh;
    } else {
        gps_fix_t g;
        gps_get(&g);
        uint32_t now = esp_timer_get_time() / 1000;
        if (g.valid && now - g.last_rx_ms < GPS_FRESH_MS) kmh = g.speed_kmh;
    }
    if (kmh < 0) {
        s_source = "--";
        return 0;
    }
    s_source = "speed";
    float kg = c->weight_kg ? c->weight_kg : 75;
    return met_for_speed(kmh) * kg / 3600.0f;
}

void health_tick(void)
{
    if (stats_paused()) return;
    s_kcal += kcal_second();

    int z = health_hr_zone_current();
    if (z >= 0) {
        s_hr_zone_ms[z] += 1000;
        if (z != s_hr_zone) {
            s_hr_zone = z;
            s_hr_zone_since_ms = stats_session_ms();
        }
    }
    z = health_pwr_zone_current();
    if (z >= 0) s_pwr_zone_ms[z] += 1000;
}
