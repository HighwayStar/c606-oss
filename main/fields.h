#pragma once
#include <stdint.h>
#include <stdbool.h>
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
    FIELD_HEADING,         /* compass direction of travel from the GPS course */
    FIELD_SUNRISE,         /* today's sunrise / sunset at the last GPS position (sun.c) */
    FIELD_SUNSET,
    FIELD_STAT_BASE = 8,   /* FIELD_STAT(stat, agg), room for 14 stats */
    /* distance / laps (trip.c) */
    FIELD_DISTANCE = 64,
    FIELD_LAPS,
    FIELD_LAP_DIST,
    FIELD_LAP_TIME,
    FIELD_LAP_SPEED,
    FIELD_PRELAP_TIME,
    FIELD_PRELAP_DIST,
    FIELD_SUNSET_IN,       /* h:mm until today's sunset */
    FIELD_COUNT
};

typedef enum { AGG_CUR, AGG_MIN, AGG_MAX, AGG_AVG, AGG_COUNT } field_agg_t;

#define FIELD_STAT(stat, agg) ((field_id_t)(FIELD_STAT_BASE + (stat) * AGG_COUNT + (agg)))
#define FIELD_STAT_END        (FIELD_STAT_BASE + STAT_COUNT * AGG_COUNT)

void fields_init(void);
const char *field_name(field_id_t id);   /* "Avg Speed" */
const char *field_unit(field_id_t id);   /* "km/h", "" for none */
/* Current text of the field ("--" when unknown). */
void field_value(field_id_t id, char *buf, size_t n);

/* Sunrise / sunset text as shown in a cell (also used by the idle screen). */
void field_fmt_sun(bool rise, char *buf, size_t n);

/* Data sources that are not statistics. */
void fields_set_battery_pct(uint8_t pct);

/* Chooser: categories = one per measured parameter + Distance, Lap, Other. */
int field_category_count(void);
const char *field_category_name(int cat);
int field_category_items(int cat, field_id_t *out, int max);
