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

static char s_name[FIELD_COUNT][24];
static uint8_t s_batt_pct;
static bool s_batt_valid;
static struct { uint8_t hh, mm, ss; uint32_t at_ms; bool valid; } s_rtc;

static const char *k_agg_prefix[AGG_COUNT] = { "", "Min ", "Max ", "Avg " };

static const struct { field_id_t id; const char *name, *unit; } k_other[] = {
    { FIELD_NONE,         "(empty)",      "" },
    { FIELD_TIME_OF_DAY,  "Time of Day",  "" },
    { FIELD_SESSION_TIME, "Session Time", "" },
    { FIELD_BATTERY_PCT,  "Battery",      "%" },
    { FIELD_SATS,         "Satellites",   "" },
};
#define N_OTHER (sizeof k_other / sizeof k_other[0])

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
    if (id >= FIELD_STAT_BASE && id < FIELD_COUNT) {
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

void fields_set_rtc(uint8_t hh, uint8_t mm, uint8_t ss)
{
    s_rtc.hh = hh; s_rtc.mm = mm; s_rtc.ss = ss;
    s_rtc.at_ms = esp_timer_get_time() / 1000;
    s_rtc.valid = true;
}

static void fmt_hms(char *buf, size_t n, uint32_t secs)
{
    snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60),
             (unsigned long)(secs % 60));
}

static void fmt_time_of_day(char *buf, size_t n)
{
    int32_t utc = -1;
    if (s_rtc.valid) {
        utc = s_rtc.hh * 3600 + s_rtc.mm * 60 + s_rtc.ss + (int32_t)((esp_timer_get_time() / 1000 - s_rtc.at_ms) / 1000);
    } else {
        gps_fix_t g;
        gps_get(&g);
        if (g.sentences && (g.hh || g.mm || g.ss)) {
            utc = g.hh * 3600 + g.mm * 60 + g.ss;
        }
    }
    if (utc < 0) {
        snprintf(buf, n, "--:--:--");
        return;
    }
    int32_t local = (utc + config_get()->tz_min * 60) % 86400;
    if (local < 0) local += 86400;
    snprintf(buf, n, "%02ld:%02ld:%02ld", (long)(local / 3600), (long)(local / 60 % 60), (long)(local % 60));
}

void field_value(field_id_t id, char *buf, size_t n)
{
    if (id >= FIELD_STAT_BASE && id < FIELD_COUNT) {
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
    default:
        buf[0] = 0;
        break;
    }
}

/* ---- chooser categories ------------------------------------------------ */

int field_category_count(void)
{
    return STAT_COUNT + 1;
}

const char *field_category_name(int cat)
{
    if (cat < STAT_COUNT) return stats_info(cat)->name;
    return "Other";
}

int field_category_items(int cat, field_id_t *out, int max)
{
    int n = 0;
    if (cat < STAT_COUNT) {
        for (int a = 0; a < AGG_COUNT && n < max; a++) out[n++] = FIELD_STAT(cat, a);
    } else {
        /* "Other": time of day first, (empty) last */
        for (size_t i = 1; i < N_OTHER && n < max; i++) out[n++] = k_other[i].id;
        if (n < max) out[n++] = FIELD_NONE;
    }
    return n;
}
