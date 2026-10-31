#include "map_zoom.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    map_zoom_state_t s;
    map_zoom_init(&s);
    assert(s.zoom == 17);
    assert(map_zoom_update(&s, 0, true, true, 0) == 17);
    assert(map_zoom_update(&s, 13, true, true, 100) == 17); /* below hysteresis */
    assert(map_zoom_update(&s, 15, true, true, 200) == 17);
    assert(map_zoom_update(&s, 15, true, true, 3199) == 17);
    assert(map_zoom_update(&s, 15, true, true, 3200) == 16);
    assert(map_zoom_update(&s, 11, true, true, 4000) == 16); /* do not hunt around 12 */
    assert(map_zoom_update(&s, 28, true, true, 5000) == 16);
    assert(map_zoom_update(&s, 28, true, true, 8000) == 15);
    assert(map_zoom_update(&s, 50, true, true, 9000) == 15);
    assert(map_zoom_update(&s, 50, true, true, 12000) == 14);
    assert(map_zoom_update(&s, 0, false, true, 13000) == 14); /* stale GPS */
    assert(map_zoom_update(&s, NAN, true, true, 14000) == 14);
    assert(map_zoom_update(&s, 0, true, true, 15000) == 14);
    assert(map_zoom_update(&s, 0, true, true, 18000) == 17);
    map_zoom_manual(&s, 12, 20000);
    assert(map_zoom_update(&s, 0, true, true, 79999) == 12);
    assert(map_zoom_update(&s, 0, true, true, 80000) == 12);
    assert(map_zoom_update(&s, 0, true, true, 83000) == 17);
    map_zoom_manual(&s, 11, 84000);
    assert(map_zoom_update(&s, 0, true, false, 200000) == 11);
    assert(map_zoom_update(&s, 0, true, true, 200100) == 11);
    assert(map_zoom_update(&s, 0, true, true, 203100) == 17);
    map_zoom_manual(&s, 99, 204000); assert(s.zoom == 17);
    map_zoom_manual(&s, -1, 205000); assert(s.zoom == 10);
    map_zoom_manual(&s, 12, UINT32_MAX - 10000);
    assert(map_zoom_update(&s, 0, true, true, 10000) == 12);
    assert(map_zoom_update(&s, 0, true, true, 60000) == 12);
    assert(map_zoom_update(&s, 0, true, true, 63000) == 17);
    puts("PASS: speed bands, dwell/hysteresis, stale data, manual override, toggle and timer wrap");
}
