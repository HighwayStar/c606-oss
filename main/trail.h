#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* The path ridden so far, drawn on the map page while riding.
 *
 * Positions are collected from every valid GPS fix while the ride is
 * running (not while paused) and kept in PSRAM as Web Mercator pixels at
 * zoom 20, like the route (route.h), so the renderer places them with one
 * multiply per point. A point is only kept once the rider moved
 * TRAIL_MIN_PX from the last kept one (a few metres: standing still does
 * not add jitter); when the buffer is full every other point is dropped
 * and the input rate halved, so a long ride keeps its outline.
 *
 * trail_reset() at ride start, trail_add() from the GPS task, the map
 * renderer reads the arrays between trail_lock() / trail_unlock(). The
 * trail stays after the ride ends, until the next one starts. */

#define TRAIL_MAX_POINTS 16384
#define TRAIL_MIN_PX     40        /* zoom-20 pixels: ~6 m at the equator, ~3.5 m at 55 deg */

void trail_reset(void);
void trail_add(double lat, double lon);
uint32_t trail_generation(void);   /* bumps on every reset and every kept point */

void trail_lock(void);
void trail_unlock(void);
size_t trail_points(const int32_t **x20, const int32_t **y20);   /* valid while locked */
