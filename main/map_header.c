#include "map_header.h"
#include <math.h>
#include <stdio.h>

void map_header_format(char *out, size_t size, double lat, double lon, bool valid)
{
    if (!out || !size) return;
    if (!valid || !isfinite(lat) || !isfinite(lon) || fabs(lat) > 90 || fabs(lon) > 180) {
        snprintf(out, size, "--.----\n---.----");
        return;
    }
    snprintf(out, size, "%c%07.4f\n%c%08.4f", lat < 0 ? 'S' : 'N', fabs(lat),
             lon < 0 ? 'W' : 'E', fabs(lon));
}
