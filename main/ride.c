#include "esp_log.h"
#include "ride.h"
#include "stats.h"
#include "tracklog.h"
#include "gps.h"
#include "trip.h"

static const char *TAG = "ride";
static ride_mode_t s_mode = RIDE_IDLE;
static ride_mode_cb_t s_cb;

static void set_mode(ride_mode_t m)
{
    s_mode = m;
    stats_set_paused(m != RIDE_RIDING);
    ESP_LOGI(TAG, "%s", m == RIDE_IDLE ? "idle" : m == RIDE_RIDING ? "riding" : "paused");
    if (s_cb) s_cb(m);
}

void ride_init(ride_mode_cb_t cb)
{
    s_cb = cb;
    stats_set_paused(true);
}

ride_mode_t ride_mode(void) { return s_mode; }
bool ride_recording(void) { return s_mode == RIDE_RIDING; }

void ride_start(void)
{
    if (s_mode != RIDE_IDLE) return;
    stats_reset();
    trip_reset();
    gps_fix_t fix;
    gps_get(&fix);
    if (tracklog_start(&fix) != ESP_OK) {
        ESP_LOGW(TAG, "no track file (SD?) - riding without recording");
    }
    set_mode(RIDE_RIDING);
}

void ride_pause(void)
{
    if (s_mode == RIDE_RIDING) {
        tracklog_flush();
        set_mode(RIDE_PAUSED);
    }
}

void ride_resume(void)
{
    if (s_mode == RIDE_PAUSED) set_mode(RIDE_RIDING);
}

void ride_toggle(void)
{
    switch (s_mode) {
    case RIDE_IDLE:   ride_start(); break;
    case RIDE_RIDING: ride_pause(); break;
    case RIDE_PAUSED: ride_resume(); break;
    }
}

void ride_end(void)
{
    if (s_mode == RIDE_IDLE) return;
    tracklog_stop();
    set_mode(RIDE_IDLE);
}
