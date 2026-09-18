#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* GPX routes / tracks for the map page.
 *
 * Files live in /sdcard/c606oss/routes/ (copy them there in USB storage
 * mode). The parser is a small scanner for <trkpt>/<rtept> lat/lon
 * attributes (elevation, time and everything else is ignored), so any
 * GPX 1.0/1.1 track or route works. Points are stored in PSRAM as Web
 * Mercator pixel coordinates at zoom 20 (int32, world = 2^28 px), which
 * lets the renderer place them with one multiply per point; very long
 * tracks are decimated to ROUTE_MAX_POINTS.
 *
 * route_load() runs in the calling task (menu, boot). The renderer reads
 * the arrays between route_lock() / route_unlock(). */

#define ROUTE_DIR        "/sdcard/c606oss/routes"
#define ROUTE_NAME_MAX   40
#define ROUTE_MAX_POINTS 16384

esp_err_t route_load(const char *name);   /* file name inside ROUTE_DIR; NULL or "" clears */
void route_clear(void);
bool route_loaded(void);
const char *route_name(void);
float route_length_m(void);
uint32_t route_generation(void);          /* bumps on every load / clear */

/* Points of the loaded route (valid while locked). */
void route_lock(void);
void route_unlock(void);
size_t route_points(const int32_t **x20, const int32_t **y20);
/* Bounding box in microdegrees (valid when loaded). */
void route_bbox(int32_t *min_lat, int32_t *min_lon, int32_t *max_lat, int32_t *max_lon);

/* Lists the .gpx files in ROUTE_DIR; returns the count (<= max). */
int route_list(char names[][ROUTE_NAME_MAX], int max);
