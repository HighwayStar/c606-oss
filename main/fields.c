/*
 * Data field catalogue: names, units and current text of everything a page
 * cell can display. See fields.h.
 */
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

#include "fields.h"
#include "gps.h"
#include "config.h"
#include "trip.h"
#include "utc.h"
#include "sun.h"
#include "route.h"
#include "health.h"
#include "shifting.h"

static char s_name[FIELD_COUNT][24];
static uint8_t s_batt_pct;
static bool s_batt_valid;

static const char *k_agg_prefix[AGG_COUNT] = { "", "Min ", "Max ", "Avg " };

static const struct { field_id_t id; const char *name, *unit; } k_other[] = {
    { FIELD_NONE,         "(empty)",      "" },
    { FIELD_TIME_OF_DAY,  "Time of Day",  "" },
    { FIELD_SESSION_TIME, "Session Time", "" },
    { FIELD_BATTERY_PCT,  "Battery",      "%" },
    { FIELD_SATS,         "Satellites",   "" },
    { FIELD_HEADING,      "Heading",      "" },
    { FIELD_SUNRISE,      "Sunrise",      "" },
    { FIELD_SUNSET,       "Sunset",       "" },
    { FIELD_SUNSET_IN,    "Sunset in",    "" },
    { FIELD_DISTANCE,     "Distance",     "km" },
    { FIELD_LAPS,         "Laps",         "" },
    { FIELD_LAP_DIST,     "Lap Dist",     "km" },
    { FIELD_LAP_TIME,     "Lap Time",     "" },
    { FIELD_LAP_SPEED,    "Lap Speed",    "km/h" },
    { FIELD_PRELAP_TIME,  "PreLap Time",  "" },
    { FIELD_PRELAP_DIST,  "PreLap Dist",  "km" },
    { FIELD_ROUTE_LEFT,   "Route Left",   "km" },
    { FIELD_CALORIES,     "Calories",     "kcal" },
    { FIELD_KCAL_H,       "Cal Rate",     "kcal/h" },
    { FIELD_HR_ZONE,      "HR Zone",      "" },
    { FIELD_HR_PCT_MAX,   "%Max HR",      "%" },
    { FIELD_HR_PCT_LTHR,  "%LTHR",        "%" },
    { FIELD_ZONE_TIME,    "Zone Time",    "" },
    { FIELD_PWR_ZONE,     "Power Zone",   "" },
    { FIELD_PWR_PCT_FTP,  "%FTP",         "%" },
    { FIELD_PWR_KG,       "Power/kg",     "W/kg" },
    { FIELD_GEAR,         "Gear",         "" },
    { FIELD_GEAR_FRONT,   "Front Gear",   "" },
    { FIELD_GEAR_COMBO,   "Gear Combo",   "" },
    { FIELD_SHIFT_BATT,   "Shift Batt",   "" },
};
#define N_OTHER (sizeof k_other / sizeof k_other[0])

/* chooser categories after the statistics */
static const field_id_t k_cat_distance[] = { FIELD_DISTANCE, FIELD_LAP_DIST, FIELD_PRELAP_DIST, FIELD_ROUTE_LEFT };
static const field_id_t k_cat_lap[]      = { FIELD_LAPS, FIELD_LAP_TIME, FIELD_LAP_SPEED, FIELD_PRELAP_TIME };
static const field_id_t k_cat_health[]   = { FIELD_CALORIES, FIELD_KCAL_H, FIELD_HR_ZONE, FIELD_HR_PCT_MAX, FIELD_HR_PCT_LTHR,
                                             FIELD_ZONE_TIME, FIELD_PWR_ZONE, FIELD_PWR_PCT_FTP, FIELD_PWR_KG };
static const field_id_t k_cat_gears[]     = { FIELD_GEAR, FIELD_GEAR_FRONT, FIELD_GEAR_COMBO, FIELD_SHIFT_BATT };
static const field_id_t k_cat_other[]    = { FIELD_TIME_OF_DAY, FIELD_SESSION_TIME, FIELD_BATTERY_PCT, FIELD_SATS, FIELD_HEADING, FIELD_SUNRISE, FIELD_SUNSET, FIELD_SUNSET_IN, FIELD_NONE };

static int other_idx(field_id_t id)
{
    for (size_t i = 0; i < N_OTHER; i++) {
        if (k_other[i].id == id) return (int)i;
    }
    return -1;
}

void fields_init(void)
{
    for (int s = 0; s < STAT_COUNT; s++) {
        for (int a = 0; a < AGG_COUNT; a++) {
            snprintf(s_name[FIELD_STAT(s, a)], sizeof s_name[0], "%s%s", k_agg_prefix[a], stats_info(s)->name);
        }
    }
    for (size_t i = 0; i < N_OTHER; i++) {
        strncpy(s_name[k_other[i].id], k_other[i].name, sizeof s_name[0] - 1);
    }
}

const char *field_name(field_id_t id)
{
    return id < FIELD_COUNT ? s_name[id] : "?";
}

const char *field_unit(field_id_t id)
{
    if (id >= FIELD_STAT_BASE && id < FIELD_STAT_END) {
        return stats_info((id - FIELD_STAT_BASE) / AGG_COUNT)->unit;
    }
    int i = other_idx(id);
    return i >= 0 ? k_other[i].unit : "";
}

void fields_set_battery_pct(uint8_t pct)
{
    s_batt_pct = pct;
    s_batt_valid = true;
}

static void fmt_hms(char *buf, size_t n, uint32_t secs)
{
    snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60),
             (unsigned long)(secs % 60));
}

static void fmt_time_of_day(char *buf, size_t n)
{
    uint32_t unix_s;
    if (!utc_now(&unix_s)) {
        snprintf(buf, n, "--:--:--");
        return;
    }
    int32_t local = (int32_t)(unix_s % 86400) + config_get()->tz_min * 60;
    local %= 86400;
    if (local < 0) local += 86400;
    snprintf(buf, n, "%02ld:%02ld:%02ld", (long)(local / 3600), (long)(local / 60 % 60), (long)(local % 60));
}

/* Sunrise (rise = true) or sunset as local HH:MM; "--:--" while the
 * position or the clock is unknown, "24h" / "none" on polar days. */
void field_fmt_sun(bool rise, char *buf, size_t n)
{
    sun_times_t s;
    if (!sun_today(&s)) {
        snprintf(buf, n, "--:--");
        return;
    }
    if (s.polar_day)   { snprintf(buf, n, "24h");  return; }
    if (s.polar_night) { snprintf(buf, n, "none"); return; }
    int32_t local = (int32_t)((rise ? s.rise_unix : s.set_unix) % 86400) + config_get()->tz_min * 60;
    local %= 86400;
    if (local < 0) local += 86400;
    snprintf(buf, n, "%02ld:%02ld", (long)(local / 3600), (long)(local / 60 % 60));
}

/* h:mm until today's sunset; "--" once the sun is down. */
static void fmt_sunset_in(char *buf, size_t n)
{
    sun_times_t s;
    uint32_t now;
    if (!sun_today(&s) || !utc_now(&now)) { snprintf(buf, n, "--:--"); return; }
    if (s.polar_day)   { snprintf(buf, n, "24h");  return; }
    if (s.polar_night) { snprintf(buf, n, "none"); return; }
    if (now >= s.set_unix) { snprintf(buf, n, "--"); return; }
    uint32_t left = (s.set_unix - now + 30) / 60;   /* minutes, rounded */
    snprintf(buf, n, "%lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
}

void field_value(field_id_t id, char *buf, size_t n)
{
    if (id >= FIELD_STAT_BASE && id < FIELD_STAT_END) {
        stat_id_t st = (id - FIELD_STAT_BASE) / AGG_COUNT;
        field_agg_t agg = (id - FIELD_STAT_BASE) % AGG_COUNT;
        stat_values_t v;
        stats_get(st, &v);
        float x = agg == AGG_MIN ? v.min : agg == AGG_MAX ? v.max : agg == AGG_AVG ? v.avg : v.cur;
        bool ok = v.valid && (agg != AGG_CUR || v.live);
        stats_format(st, x, ok, buf, n);
        return;
    }
    switch (id) {
    case FIELD_TIME_OF_DAY:
        fmt_time_of_day(buf, n);
        break;
    case FIELD_SESSION_TIME:
        fmt_hms(buf, n, stats_session_ms() / 1000);
        break;
    case FIELD_BATTERY_PCT:
        if (s_batt_valid) snprintf(buf, n, "%u", s_batt_pct);
        else snprintf(buf, n, "--");
        break;
    case FIELD_SATS: {
        gps_fix_t g;
        gps_get(&g);
        if (g.baud) snprintf(buf, n, "%u/%u", g.sats_used, g.sats_in_view);
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_HEADING: {
        static const char *k_dir[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        gps_fix_t g;
        gps_get(&g);
        if (g.valid && g.speed_kmh > 2.0f) snprintf(buf, n, "%s", k_dir[((int)(g.course_deg + 22.5f) / 45) & 7]);
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_SUNRISE:     field_fmt_sun(true, buf, n); break;
    case FIELD_SUNSET:      field_fmt_sun(false, buf, n); break;
    case FIELD_SUNSET_IN:   fmt_sunset_in(buf, n); break;
    case FIELD_DISTANCE:    snprintf(buf, n, "%.2f", trip_distance_m() / 1000); break;
    case FIELD_LAP_DIST:    snprintf(buf, n, "%.2f", trip_lap_distance_m() / 1000); break;
    case FIELD_LAPS:        snprintf(buf, n, "%lu", (unsigned long)trip_laps()); break;
    case FIELD_LAP_TIME:    fmt_hms(buf, n, trip_lap_time_ms() / 1000); break;
    case FIELD_LAP_SPEED:   snprintf(buf, n, "%.1f", trip_lap_avg_kmh()); break;
    case FIELD_PRELAP_TIME:
        if (trip_laps()) fmt_hms(buf, n, trip_prev_lap_time_ms() / 1000);
        else snprintf(buf, n, "--");
        break;
    case FIELD_PRELAP_DIST:
        if (trip_laps()) snprintf(buf, n, "%.2f", trip_prev_lap_distance_m() / 1000);
        else snprintf(buf, n, "--");
        break;
    case FIELD_ROUTE_LEFT: {
        float left;
        if (route_remaining(&left, NULL)) snprintf(buf, n, left < 100000 ? "%.1f" : "%.0f", left / 1000);
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_CALORIES:    snprintf(buf, n, "%.0f", health_kcal()); break;
    case FIELD_KCAL_H:
        if (health_kcal_per_h() > 0) snprintf(buf, n, "%.0f", health_kcal_per_h());
        else snprintf(buf, n, "--");
        break;
    case FIELD_HR_ZONE: {
        int z = health_hr_zone_current();
        if (z >= 0) snprintf(buf, n, "Z%d", z);
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_HR_PCT_MAX:
    case FIELD_HR_PCT_LTHR: {
        stat_values_t v;
        stats_get(STAT_HR, &v);
        int ref = id == FIELD_HR_PCT_MAX ? health_max_hr() : health_lthr();
        if (v.valid && v.live && ref) snprintf(buf, n, "%d", (int)(v.cur * 100 / ref + 0.5f));
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_ZONE_TIME:
        if (health_hr_zone_current() >= 0) fmt_hms(buf, n, health_zone_since_ms() / 1000);
        else snprintf(buf, n, "--");
        break;
    case FIELD_PWR_ZONE: {
        int z = health_pwr_zone_current();
        if (z >= 0) snprintf(buf, n, "Z%d", z);
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_PWR_PCT_FTP: {
        stat_values_t v;
        stats_get(STAT_POWER, &v);
        if (v.valid && v.live && health_pwr_zones_available()) snprintf(buf, n, "%d", (int)(v.cur * 100 / config_get()->ftp_w + 0.5f));
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_PWR_KG: {
        stat_values_t v;
        stats_get(STAT_POWER, &v);
        if (v.valid && v.live && config_get()->weight_kg) snprintf(buf, n, "%.1f", v.cur / config_get()->weight_kg);
        else snprintf(buf, n, "--");
        break;
    }
    case FIELD_GEAR:
    case FIELD_GEAR_FRONT:
    case FIELD_GEAR_COMBO: {
        shifting_t g;
        shifting_get(&g);
        bool live = shifting_live();
        bool front = live && g.front_valid, rear = live && g.rear_valid;
        /* the total is unknown until a Di2 has answered with its gear counts */
        if (id == FIELD_GEAR) {
            if (rear && g.rear_total) snprintf(buf, n, "%u/%u", g.rear, g.rear_total);
            else if (rear) snprintf(buf, n, "%u/--", g.rear);
            else snprintf(buf, n, "--");
        } else if (id == FIELD_GEAR_FRONT) {
            if (front && g.front_total) snprintf(buf, n, "%u/%u", g.front, g.front_total);
            else if (front) snprintf(buf, n, "%u/--", g.front);
            else snprintf(buf, n, "--");
        } else if (front && rear) {
            snprintf(buf, n, "%ux%u", g.front, g.rear);
        } else if (rear) {
            snprintf(buf, n, "%u", g.rear);   /* 1x drivetrain: no front to show */
        } else {
            snprintf(buf, n, "--");
        }
        break;
    }
    case FIELD_SHIFT_BATT: {
        shifting_t g;
        shifting_get(&g);
        if (g.batt_valid && g.batt_pct) snprintf(buf, n, "%u%%", g.batt_pct);     /* Di2 */
        else if (g.batt_valid && g.batt_v > 0) snprintf(buf, n, "%.1fV", g.batt_v);
        else snprintf(buf, n, "%s", shifting_batt_text());
        break;
    }
    default:
        buf[0] = 0;
        break;
    }
}

/* ---- chooser categories ------------------------------------------------ */

int field_category_count(void)
{
    return STAT_COUNT + 5;
}

const char *field_category_name(int cat)
{
    if (cat < STAT_COUNT) return stats_info(cat)->name;
    switch (cat - STAT_COUNT) {
    case 0: return "Distance";
    case 1: return "Lap";
    case 2: return "Health";
    case 3: return "Gears";
    default: return "Other";
    }
}

int field_category_items(int cat, field_id_t *out, int max)
{
    int n = 0;
    if (cat < STAT_COUNT) {
        for (int a = 0; a < AGG_COUNT && n < max; a++) out[n++] = FIELD_STAT(cat, a);
        return n;
    }
    const field_id_t *list;
    int cnt;
    switch (cat - STAT_COUNT) {
    case 0:  list = k_cat_distance; cnt = sizeof k_cat_distance; break;
    case 1:  list = k_cat_lap;      cnt = sizeof k_cat_lap; break;
    case 2:  list = k_cat_health;   cnt = sizeof k_cat_health; break;
    case 3:  list = k_cat_gears;    cnt = sizeof k_cat_gears; break;
    default: list = k_cat_other;    cnt = sizeof k_cat_other; break;
    }
    for (int i = 0; i < cnt && n < max; i++) out[n++] = list[i];
    return n;
}
