#pragma once
#include <stdint.h>
#include <stddef.h>
#include "stats.h"

/* Data fields: everything a page cell can show. Most are a statistic of a
 * measured parameter (current / min / max / avg of stats.h); a few are
 * "other" values (time of day, session time, ...). Field ids are stored in
 * the page configuration, so keep them stable. */

typedef uint8_t field_id_t;

enum {
    FIELD_NONE = 0,
    FIELD_TIME_OF_DAY,     /* nRF RTC (UTC + configured offset), GPS fallback */
    FIELD_SESSION_TIME,    /* since the last stats reset */
    FIELD_BATTERY_PCT,
    FIELD_SATS,            /* GPS satellites used / in view */
    FIELD_STAT_BASE = 8,   /* FIELD_STAT(stat, agg) */
};

typedef enum { AGG_CUR, AGG_MIN, AGG_MAX, AGG_AVG, AGG_COUNT } field_agg_t;

#define FIELD_STAT(stat, agg) ((field_id_t)(FIELD_STAT_BASE + (stat) * AGG_COUNT + (agg)))
#define FIELD_COUNT           (FIELD_STAT_BASE + STAT_COUNT * AGG_COUNT)

void fields_init(void);
const char *field_name(field_id_t id);   /* "Avg Speed" */
const char *field_unit(field_id_t id);   /* "km/h", "" for none */
/* Current text of the field ("--" when unknown). */
void field_value(field_id_t id, char *buf, size_t n);

/* Data sources that are not statistics. */
void fields_set_battery_pct(uint8_t pct);
void fields_set_rtc(uint8_t hh, uint8_t mm, uint8_t ss);   /* UTC */

/* Chooser: categories = one per measured parameter + "Other". */
int field_category_count(void);
const char *field_category_name(int cat);
int field_category_items(int cat, field_id_t *out, int max);
