#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAP_ZOOM_MIN 10
#define MAP_ZOOM_MAX 17
#define MAP_ZOOM_MANUAL_MS 60000u
#define MAP_ZOOM_DWELL_MS 3000u

typedef struct {
    uint8_t zoom, candidate;
    uint32_t candidate_since, manual_since;
    bool manual, auto_was_enabled;
} map_zoom_state_t;

void map_zoom_init(map_zoom_state_t *s);
void map_zoom_manual(map_zoom_state_t *s, int zoom, uint32_t now_ms);
uint8_t map_zoom_update(map_zoom_state_t *s, float speed_kmh, bool valid,
                        bool enabled, uint32_t now_ms);
