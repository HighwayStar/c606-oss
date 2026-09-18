#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/* Map page: draws the vendor's vector maps (MAP/ directory on the eMMC, see
 * mapfile.h) around the current GPS position into an LVGL canvas.
 *
 * Rendering (file reads + line drawing) runs in its own low-priority task
 * into a back buffer; the LVGL task only copies the finished frame. The
 * view re-renders when the position moved by more than a few pixels or the
 * zoom changed. Zoom 10..17 (data above the file's top zoom is reused,
 * scaled). Maps whose file name contains "china" are treated as GCJ-02 and
 * the WGS-84 fix is shifted to match them. */

esp_err_t mapview_init(void);          /* after the card is mounted: scans MAP/ and opens the maps */
void mapview_close(void);              /* before the card is unmounted */
bool mapview_available(void);          /* at least one map opened */
int mapview_map_count(void);

/* Creates the canvas and the zoom buttons inside `parent` at (0, y), w x h.
 * Called under the LVGL lock. Only one instance is supported. */
lv_obj_t *mapview_create(lv_obj_t *parent, int32_t y, int32_t w, int32_t h);
/* Moves / shrinks the map area (h <= the h given to mapview_create), e.g.
 * when the field strip below it changes; also re-applies the theme colours
 * and puts the map widgets above anything built since. */
void mapview_set_area(int32_t y, int32_t h);
void mapview_set_visible(bool visible);   /* the page is shown / hidden */
void mapview_zoom_by(int delta);
uint8_t mapview_zoom(void);

/* Position to centre on (WGS-84). A fixed override (developer console
 * `pos lat lon`, `pos off`) wins over the GPS. */
void mapview_set_position(double lat, double lon, bool valid);
void mapview_set_override(double lat, double lon, bool on);

/* For the header: "z15 1200 ways 87 ms" style status of the last render. */
const char *mapview_status(void);
