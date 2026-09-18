#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gps.h"

/* Ride distance and auto laps. Distance comes from the ANT+ wheel sensor
 * when one is live (revs x wheel circumference), otherwise from consecutive
 * GPS fixes. A lap ends every `lap length` metres (config, 0 = off).
 * Accumulation stops while the statistics are paused (ride paused / idle). */

void trip_reset(void);                    /* at ride start */
uint32_t trip_lap_manual(void);           /* end the current lap now; returns the new lap number */
void trip_gps(const gps_fix_t *fix);      /* every GPS update */
void trip_wheel(uint32_t revs, bool live);/* every ANT update */

float    trip_distance_m(void);
uint32_t trip_laps(void);                 /* completed laps */
float    trip_lap_distance_m(void);       /* current lap */
uint32_t trip_lap_time_ms(void);
float    trip_lap_avg_kmh(void);
uint32_t trip_prev_lap_time_ms(void);     /* last completed lap, 0 if none */
float    trip_prev_lap_distance_m(void);
