#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* GPX routes / tracks for the map page.
 *
 * Files live in /sdcard/c606oss/routes/ (copy them there in USB storage
 * mode). The parser is a small scanner for <trkpt>/<rtept> lat/lon
 * attributes plus the <ele> child (time and everything else is ignored),
 * so any GPX 1.0/1.1 track or route works. Points are stored in PSRAM as Web
 * Mercator pixel coordinates at zoom 20 (int32, world = 2^28 px), which
 * lets the renderer place them with one multiply per point; very long
 * tracks are decimated to ROUTE_MAX_POINTS.
 *
 * route_load() runs in the calling task (menu, boot). The renderer reads
 * the arrays between route_lock() / route_unlock(). */

#define ROUTE_DIR        "/sdcard/c606oss/routes"
#define ROUTE_NAME_MAX   40
#define ROUTE_MAX_POINTS 16384

/* File name inside ROUTE_DIR; NULL or "" clears. `reverse` loads the
 * points end to start (ride the track backwards). */
esp_err_t route_load(const char *name, bool reverse);
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

/* Position on the loaded route. route_track() (every valid GPS fix) finds
 * the nearest point of the track — preferring the leg matched last time, so
 * an out-and-back track does not flip to the other leg — and
 * route_remaining() gives the track distance from there to the end, plus
 * how far off the track the rider is. False until a route is loaded and a
 * fix has been seen. */
void route_track(double lat, double lon);
bool route_remaining(float *remaining_m, float *off_track_m);

/* Summary of a file for the preview screen, without touching the loaded
 * route: length, climb / descent (when the points carry <ele>, with a
 * 5 m hysteresis so GPS noise does not add up), bounding box and a thinned
 * copy of the points for a thumbnail. */
typedef struct {
    uint32_t npoints;                 /* points in the file */
    float len_m;
    bool has_ele;
    float climb_m, descent_m, min_ele_m, max_ele_m;
    int32_t min_lat, min_lon, max_lat, max_lon;   /* microdegrees */
    int32_t *x20, *y20;               /* thinned points, zoom-20 pixels (PSRAM) */
    size_t n;
} route_info_t;

esp_err_t route_scan(const char *name, size_t max_points, route_info_t *info);
void route_info_free(route_info_t *info);

/* Lists the .gpx files in ROUTE_DIR; returns the count (<= max). */
int route_list(char names[][ROUTE_NAME_MAX], int max);
