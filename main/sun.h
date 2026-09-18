#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Sunrise / sunset for the current position and local date. The position
 * comes from the GPS (last valid fix); it is also remembered in NVS so the
 * times are available right after power-on, before the receiver has a
 * fix. The date is the local calendar day of utc_now() + the configured
 * time zone. */

typedef struct {
    uint32_t rise_unix;    /* UTC unix seconds; valid when !polar_day && !polar_night */
    uint32_t set_unix;
    bool polar_day;        /* the sun never sets on this day */
    bool polar_night;      /* the sun never rises on this day */
} sun_times_t;

void sun_init(void);                          /* after config_load() (NVS) */
void sun_set_position(double lat, double lon);
bool sun_has_position(void);

/* Today's times; false while the position or the clock is unknown. The
 * result is cached, so calling it a few times a second is fine. */
bool sun_today(sun_times_t *out);

/* Daytime = between sunrise and sunset (polar day counts as day). False
 * while unknown. */
bool sun_is_day(bool *day);

/* Pure computation for one date (unix seconds anywhere inside the UTC day
 * that contains local noon) and position; exposed for host tests. */
void sun_compute(uint32_t day_unix, double lat, double lon, sun_times_t *out);
