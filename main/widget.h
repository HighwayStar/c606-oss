#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "stats.h"

/* A data field showing one measured parameter: title + unit, the current
 * value in big digits and its session min / max / avg. Pages are a 2-column
 * grid; a widget spans 1 or 2 columns. The same parameter can be placed on
 * any number of pages, each widget is independent. */

#define WIDGET_COL_W   116
#define WIDGET_GAP     4
#define WIDGET_ROW_H   84
#define WIDGET_COLS    2

typedef struct {
    stat_id_t id;
    uint8_t cols;              /* 1 or 2 */
    lv_obj_t *cont;
    lv_obj_t *cur, *min, *max, *avg;
    char last[4][16];          /* last text of cur/min/max/avg, to skip redundant redraws */
} widget_t;

/* Creates the widget at grid position (col, row) inside parent; y0 is the
 * grid's top edge. Call with the LVGL lock held. */
void widget_create(widget_t *w, lv_obj_t *parent, stat_id_t id, uint8_t cols, uint8_t col, uint8_t row, int y0);

/* Pulls the latest values from the stats module. LVGL lock must be held. */
void widget_refresh(widget_t *w);
