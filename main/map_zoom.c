#include "map_zoom.h"
#include <math.h>

void map_zoom_init(map_zoom_state_t *s)
{
    *s = (map_zoom_state_t){ .zoom = MAP_ZOOM_MAX, .candidate = MAP_ZOOM_MAX,
                            .auto_was_enabled = true };
}

void map_zoom_manual(map_zoom_state_t *s, int zoom, uint32_t now_ms)
{
    if (zoom < MAP_ZOOM_MIN) zoom = MAP_ZOOM_MIN;
    if (zoom > MAP_ZOOM_MAX) zoom = MAP_ZOOM_MAX;
    s->zoom = s->candidate = zoom;
    s->candidate_since = s->manual_since = now_ms;
    s->manual = true;
}

uint8_t map_zoom_update(map_zoom_state_t *s, float speed, bool valid, bool enabled, uint32_t now)
{
    if (!enabled) {
        s->auto_was_enabled = false;
        s->candidate = s->zoom;
        return s->zoom;
    }
    if (!s->auto_was_enabled) {
        s->auto_was_enabled = true;
        s->manual = false;
        s->candidate = s->zoom;
    }
    if (s->manual) {
        if (now - s->manual_since < MAP_ZOOM_MANUAL_MS) return s->zoom;
        s->manual = false;
        s->candidate = s->zoom;
    }
    if (!valid || !isfinite(speed) || speed < 0) {
        s->candidate = s->zoom;
        return s->zoom;
    }
    uint8_t target = speed < 12 ? 17 : speed < 25 ? 16 : speed < 40 ? 15 : 14;
    /* Two km/h hysteresis at each boundary; no GPS-noise zoom oscillation. */
    if (target < s->zoom) {
        float boundary = s->zoom == 17 ? 12 : s->zoom == 16 ? 25 : s->zoom == 15 ? 40 : -100;
        if (speed < boundary + 2) target = s->zoom;
    } else if (target > s->zoom) {
        float boundary = s->zoom == 16 ? 12 : s->zoom == 15 ? 25 : s->zoom == 14 ? 40 : 1000;
        if (speed > boundary - 2) target = s->zoom;
    }
    if (target != s->candidate) {
        s->candidate = target;
        s->candidate_since = now;
    } else if (target != s->zoom && now - s->candidate_since >= MAP_ZOOM_DWELL_MS) {
        s->zoom = target;
    }
    return s->zoom;
}
