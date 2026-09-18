#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Wall-clock time (UTC). Sources, best first: the nRF's RTC (streamed at
 * 5 Hz once its power-on gesture happened, see README), then the GPS date
 * and time from RMC. Either is carried forward with the ESP timer between
 * updates. */

/* nRF RTC frame payload: struct tm style, month 0-based, year since 1900,
 * day in the low 5 bits. */
void utc_set_rtc(uint8_t ss, uint8_t mm, uint8_t hh, uint8_t dd, uint8_t mon0, uint8_t yy1900);

/* Unix seconds now; false while no source has delivered a date and time. */
bool utc_now(uint32_t *unix_s);

/* Civil date/time (year 2-digit like gps_fix_t) -> unix seconds. */
uint32_t utc_from_civil(uint16_t year, uint8_t mon, uint8_t day, uint8_t hh, uint8_t mm, uint8_t ss);
