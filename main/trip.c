#include <math.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "trip.h"
#include "stats.h"
#include "config.h"
#include "ant.h"

static const char *TAG = "trip";

#define GPS_MIN_SPEED_KMH 2.0f     /* below this a fix is treated as standing still */
#define GPS_MAX_STEP_M    200.0f   /* larger jumps are fix glitches */
#define WHEEL_HOLDOFF_MS  15000    /* GPS takes over this long after the last wheel update */

static float s_dist_m, s_lap_start_m, s_prev_lap_m;
static uint32_t s_laps, s_lap_start_ms, s_prev_lap_ms;
static struct { bool valid; double lat, lon; } s_last_fix;
static struct { bool valid; uint32_t revs, at_ms; } s_wheel;

void trip_reset(void)
{
    s_dist_m = s_lap_start_m = s_prev_lap_m = 0;
    s_laps = 0;
    s_lap_start_ms = s_prev_lap_ms = 0;
    s_last_fix.valid = false;
    s_wheel.valid = false;
}

static void add_distance(float m)
{
    s_dist_m += m;
    uint16_t lap_len = config_get()->lap_len_m;
    while (lap_len && s_dist_m - s_lap_start_m >= lap_len) {
        uint32_t now = stats_session_ms();
        s_laps++;
        s_prev_lap_m = lap_len;
        s_prev_lap_ms = now - s_lap_start_ms;
        s_lap_start_m += lap_len;       /* the overshoot counts towards the next lap */
        s_lap_start_ms = now;
        ESP_LOGI(TAG, "lap %lu: %lu ms", (unsigned long)s_laps, (unsigned long)s_prev_lap_ms);
    }
}

static float haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0, d2r = M_PI / 180.0;
    double dlat = (lat2 - lat1) * d2r, dlon = (lon2 - lon1) * d2r;
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dlon / 2) * sin(dlon / 2);
    return (float)(2 * R * atan2(sqrt(a), sqrt(1 - a)));
}

void trip_gps(const gps_fix_t *fix)
{
    if (!fix->valid) {
        s_last_fix.valid = false;
        return;
    }
    bool first = !s_last_fix.valid;
    double lat = s_last_fix.lat, lon = s_last_fix.lon;
    s_last_fix.valid = true;
    s_last_fix.lat = fix->lat;
    s_last_fix.lon = fix->lon;
    if (first || stats_paused()) return;
    /* the wheel sensor is the better source while it is delivering */
    if (s_wheel.valid && fix->last_rx_ms - s_wheel.at_ms < WHEEL_HOLDOFF_MS) return;
    if (fix->speed_kmh < GPS_MIN_SPEED_KMH) return;
    float step = haversine_m(lat, lon, fix->lat, fix->lon);
    if (step > 0 && step < GPS_MAX_STEP_M) add_distance(step);
}

void trip_wheel(uint32_t revs, bool live)
{
    if (!live) {
        s_wheel.valid = false;
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_wheel.valid && !stats_paused() && revs != s_wheel.revs) {
        add_distance((float)(revs - s_wheel.revs) * ANT_WHEEL_CIRC_M);
    }
    s_wheel.valid = true;
    s_wheel.revs = revs;
    s_wheel.at_ms = now;
}

float trip_distance_m(void) { return s_dist_m; }
uint32_t trip_laps(void) { return s_laps; }
float trip_lap_distance_m(void) { return s_dist_m - s_lap_start_m; }
uint32_t trip_lap_time_ms(void) { return stats_session_ms() - s_lap_start_ms; }
float trip_lap_avg_kmh(void)
{
    uint32_t t = trip_lap_time_ms();
    return t > 1000 ? trip_lap_distance_m() / (t / 1000.0f) * 3.6f : 0;
}
uint32_t trip_prev_lap_time_ms(void) { return s_prev_lap_ms; }
float trip_prev_lap_distance_m(void) { return s_prev_lap_m; }
