#pragma once
#include <stddef.h>
#include <stdint.h>

/* Returns complete NMEA frame length, or 0 for invalid time / small buffer. */
size_t gps_aid_time_frame(uint32_t utc, char *out, size_t capacity);
