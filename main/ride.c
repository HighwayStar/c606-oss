#include "esp_log.h"
#include "ride.h"
#include "stats.h"
#include "tracklog.h"
#include "gps.h"
#include "trip.h"
#include "config.h"

static const char *TAG = "ride";
static ride_mode_t s_mode = RIDE_IDLE;
static ride_mode_cb_t s_cb;
static bool s_auto_paused;
static uint8_t s_still_s;          /* consecutive seconds below the pause speed */

#define AUTO_PAUSE_KMH   1.5f      /* standing still below this ... */
#define AUTO_PAUSE_S     3         /* ... for this long pauses */
#define AUTO_RESUME_KMH  3.0f      /* moving faster than this resumes */

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
    s_auto_paused = false;
    s_still_s = 0;
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
        s_auto_paused = false;
        tracklog_flush();
        set_mode(RIDE_PAUSED);
    }
}

void ride_resume(void)
{
    if (s_mode == RIDE_PAUSED) {
        s_auto_paused = false;
        s_still_s = 0;
        set_mode(RIDE_RIDING);
    }
}

bool ride_auto_paused(void) { return s_mode == RIDE_PAUSED && s_auto_paused; }

void ride_speed(float kmh, bool valid)
{
    if (!config_get()->auto_pause || !valid) {
        s_still_s = 0;
        return;
    }
    if (s_mode == RIDE_RIDING) {
        if (kmh < AUTO_PAUSE_KMH) {
            if (++s_still_s >= AUTO_PAUSE_S) {
                ESP_LOGI(TAG, "auto pause");
                ride_pause();
                s_auto_paused = true;
            }
        } else {
            s_still_s = 0;
        }
    } else if (s_mode == RIDE_PAUSED && s_auto_paused && kmh > AUTO_RESUME_KMH) {
        ESP_LOGI(TAG, "auto resume");
        ride_resume();
    }
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
