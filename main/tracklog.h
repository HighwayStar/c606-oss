#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "gps.h"

/* CSV track logger: one line per GPS update with a fix. Files live in
 * /sdcard/c606oss/ and are named from GPS UTC date/time. */
esp_err_t tracklog_start(const gps_fix_t *fix);
void tracklog_stop(void);
void tracklog_flush(void);   /* push buffered points to the card */
bool tracklog_active(void);
uint32_t tracklog_points(void);
const char *tracklog_filename(void);

/* Call on every GPS update; ignored when not recording or without a fix. */
void tracklog_point(const gps_fix_t *fix, int16_t temp_c100, uint32_t pressure_pa100);
