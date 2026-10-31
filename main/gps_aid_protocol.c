#include "gps_aid_protocol.h"
#include <stdio.h>
#include <time.h>

size_t gps_aid_time_frame(uint32_t utc, char *out, size_t capacity)
{
    if (!out || !capacity || utc < 315532800) return 0; /* PAIR590: >= 1980 */
    time_t stamp = utc;
    struct tm t;
    if (!gmtime_r(&stamp, &t)) return 0;
    char body[80];
    int n = snprintf(body, sizeof body, "PAIR590,%d,%d,%d,%d,%d,%d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
    if (n < 0 || (size_t)n >= sizeof body || capacity < (size_t)n + 7) return 0;
    uint8_t checksum = 0;
    for (const char *p = body; *p; ++p) checksum ^= (uint8_t)*p;
    return (size_t)snprintf(out, capacity, "$%s*%02X\r\n", body, checksum);
}
