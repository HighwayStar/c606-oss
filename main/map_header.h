#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Two ASCII lines, <= 9 characters per line (including hemisphere).
 * Four decimal places, about 11 m latitude resolution. No render statistics. */
void map_header_format(char *out, size_t size, double lat, double lon, bool valid);
