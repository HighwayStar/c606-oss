/*
 * Session statistics for measured parameters.
 *
 * The average is time-weighted: each sample's value counts for the interval
 * until the next sample (capped at AVG_GAP_CAP_MS so a sensor dropout does
 * not stretch the last reading over the gap). That makes it independent of
 * the producers' update rates (GPS 2/s, ANT pages ~4/s, baro 5/s).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

#include "stats.h"

#define AVG_GAP_CAP_MS 5000

static const stat_info_t k_info[STAT_COUNT] = {
    [STAT_SPEED]     = { "Speed",    "km/h", 1, false, 3000 },
    [STAT_ALTITUDE]  = { "Altitude", "m",    0, false, 3000 },
    [STAT_HR]        = { "HR",       "bpm",  0, true,  6000 },
    [STAT_CADENCE]   = { "Cadence",  "rpm",  0, true,  6000 },
    [STAT_ANT_SPEED] = { "Spd Sensor", "km/h", 1, false, 6000 },
    [STAT_POWER]     = { "Power",    "W",    0, false, 6000 },
    [STAT_TEMP]      = { "Temp",     "C",    1, false, 5000 },
    [STAT_PRESSURE]  = { "Pressure", "hPa",  0, false, 5000 },
    [STAT_BATTERY]   = { "Battery",  "mV",   0, false, 5000 },
};

typedef struct {
    float cur, min, max;
    double sum_vdt;        /* integral of value over time */
    double sum_dt;         /* ms */
    uint32_t samples;
    uint32_t last_ms;
    bool valid;
} stat_t;

static stat_t s_st[STAT_COUNT];
static uint32_t s_reset_ms;
static bool s_paused;
static uint32_t s_paused_since, s_paused_total;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return esp_timer_get_time() / 1000; }

void stats_init(void)
{
    stats_reset();
}

void stats_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_st, 0, sizeof s_st);
    s_reset_ms = now_ms();
    s_paused_total = 0;
    s_paused_since = s_reset_ms;
    portEXIT_CRITICAL(&s_mux);
}

void stats_set_paused(bool paused)
{
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    if (paused && !s_paused) {
        s_paused_since = now;
    } else if (!paused && s_paused) {
        s_paused_total += now - s_paused_since;
        /* the pause must not count as an interval for the averages */
        for (int i = 0; i < STAT_COUNT; i++) s_st[i].last_ms = now;
    }
    s_paused = paused;
    portEXIT_CRITICAL(&s_mux);
}

bool stats_paused(void) { return s_paused; }

uint32_t stats_session_ms(void)
{
    uint32_t now = now_ms();
    return now - s_reset_ms - s_paused_total - (s_paused ? now - s_paused_since : 0);
}

const stat_info_t *stats_info(stat_id_t id)
{
    return &k_info[id < STAT_COUNT ? id : 0];
}

void stats_update(stat_id_t id, float v)
{
    if (id >= STAT_COUNT) return;
    stat_t *s = &s_st[id];
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    if (s_paused) {
        /* keep the display live, freeze the statistics */
        s->cur = v;
        s->last_ms = now;
        if (!s->valid) { s->min = s->max = v; s->valid = true; }
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (!s->valid) {
        s->min = s->max = v;
        s->valid = true;
    } else {
        if (v < s->min) s->min = v;
        if (v > s->max) s->max = v;
        /* the previous value was current for [last_ms, now) */
        uint32_t dt = now - s->last_ms;
        if (dt > AVG_GAP_CAP_MS) dt = AVG_GAP_CAP_MS;
        if (!(k_info[id].avg_skip_zero && s->cur == 0.0f)) {
            s->sum_vdt += (double)s->cur * dt;
            s->sum_dt += dt;
        }
    }
    s->cur = v;
    s->last_ms = now;
    s->samples++;
    portEXIT_CRITICAL(&s_mux);
}

void stats_get(stat_id_t id, stat_values_t *out)
{
    memset(out, 0, sizeof *out);
    if (id >= STAT_COUNT) return;
    stat_t *s = &s_st[id];
    portENTER_CRITICAL(&s_mux);
    stat_t c = *s;
    portEXIT_CRITICAL(&s_mux);
    out->valid = c.valid;
    out->samples = c.samples;
    if (!c.valid) return;
    out->cur = c.cur;
    out->min = c.min;
    out->max = c.max;
    /* until a second sample exists the average is the only value we have;
     * with skip-zero and only zeros so far it stays 0 */
    out->avg = c.sum_dt > 0 ? (float)(c.sum_vdt / c.sum_dt) : c.cur;
    out->live = now_ms() - c.last_ms < k_info[id].stale_ms;
}

void stats_format(stat_id_t id, float v, bool valid, char *buf, size_t n)
{
    if (!valid) {
        snprintf(buf, n, "--");
    } else {
        snprintf(buf, n, "%.*f", stats_info(id)->decimals, v);
    }
}
