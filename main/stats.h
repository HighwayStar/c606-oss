#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Session statistics (current / min / max / avg) for every measured
 * parameter. Producers call stats_update() from any task; the UI reads
 * snapshots with stats_get(). Everything restarts at stats_reset(), which is
 * also called when a recording starts. */

typedef enum {
    STAT_SPEED,       /* GPS ground speed, km/h */
    STAT_ALTITUDE,    /* GPS altitude, m */
    STAT_HR,          /* ANT+ heart rate, bpm */
    STAT_CADENCE,     /* ANT+ cadence, rpm */
    STAT_ANT_SPEED,   /* ANT+ wheel sensor speed, km/h */
    STAT_POWER,       /* ANT+ power, W */
    STAT_TEMP,        /* nRF barometer temperature, C */
    STAT_PRESSURE,    /* nRF barometer pressure, hPa */
    STAT_BATTERY,     /* battery voltage, mV */
    STAT_COUNT
} stat_id_t;

typedef struct {
    const char *name;
    const char *unit;
    uint8_t decimals;      /* for display */
    bool avg_skip_zero;    /* zero samples do not count towards the average */
    uint32_t stale_ms;     /* "current" is shown as "--" after this long without an update */
} stat_info_t;

typedef struct {
    float cur, min, max, avg;
    bool valid;            /* at least one sample since the last reset */
    bool live;             /* cur was updated less than stale_ms ago */
    uint32_t samples;
} stat_values_t;

void stats_init(void);
void stats_update(stat_id_t id, float value);
void stats_get(stat_id_t id, stat_values_t *out);
void stats_reset(void);
/* While paused, samples only update the current value; min/max/avg and the
 * session time stand still. */
void stats_set_paused(bool paused);
bool stats_paused(void);
uint32_t stats_session_ms(void);      /* time since the last reset, minus pauses */
const stat_info_t *stats_info(stat_id_t id);
/* "%.<decimals>f" of v; "--" when !valid. */
void stats_format(stat_id_t id, float v, bool valid, char *buf, size_t n);
