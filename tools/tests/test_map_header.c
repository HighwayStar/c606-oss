#include "map_header.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char out[24];
    map_header_format(out, sizeof out, 60.00382, 30.24417, true);
    assert(!strcmp(out, "N60.0038\nE030.2442"));
    map_header_format(out, sizeof out, -90, -180, true);
    assert(!strcmp(out, "S90.0000\nW180.0000"));
    assert(strchr(out, '\n') - out <= 9 && strlen(strchr(out, '\n') + 1) <= 9);
    map_header_format(out, sizeof out, 90.0001, 0, true);
    assert(!strcmp(out, "--.----\n---.----"));
    map_header_format(out, sizeof out, NAN, 0, true);
    assert(!strcmp(out, "--.----\n---.----"));
    map_header_format(out, sizeof out, 60, 30, false);
    assert(!strcmp(out, "--.----\n---.----"));
    memset(out, 'X', sizeof out);
    map_header_format(out, 4, 60, 30, true);
    assert(out[3] == 0 && out[4] == 'X');
    map_header_format(NULL, 0, 0, 0, true);
    puts("PASS: compact coordinates, hemispheres, range/NaN/no-fix and bounds");
}
