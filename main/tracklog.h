#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Ride recorder: writes a standard FIT activity file (file_id, file_creator,
 * device_info, timer events, one record per second, a lap message per lap,
 * session and activity) to /sdcard/c606oss/<local date-time>.fit.
 *
 * Driven by the ride state machine: tracklog_start() at ride start,
 * tracklog_pause() / tracklog_resume() on pauses, tracklog_stop() at the
 * end (also called before USB storage mode takes the card away). The main
 * loop calls tracklog_tick() about once a second; it writes a record while
 * the ride is running and closes laps as trip.c completes them.
 *
 * The file is only created once the UTC date and time are known (nRF RTC or
 * GPS, see utc.h); until then the start is pending. Records are timestamped
 * from the ESP timer relative to that start, so they never jump. */
esp_err_t tracklog_start(void);
void tracklog_pause(void);
void tracklog_resume(void);
void tracklog_stop(void);
void tracklog_tick(void);
void tracklog_flush(void);   /* push buffered data to the card */
bool tracklog_active(void);  /* recording, or start pending */
uint32_t tracklog_points(void);
const char *tracklog_filename(void);
