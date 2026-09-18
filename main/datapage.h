#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "config.h"
#include "layouts.h"

/* Renders one configured data page: the layout's cells, each with the
 * field name on top and the value (auto-sized font) with its unit below.
 * Used for the live pages and for the previews inside the settings menu.
 * All calls with the LVGL lock held. */

typedef struct datapage datapage_t;
typedef void (*datapage_click_cb_t)(datapage_t *dp, int cell, void *ctx);

struct datapage {
    lv_obj_t *cont;
    const page_cfg_t *cfg;
    int ncells, w, h, hl;
    struct {
        lv_obj_t *box, *name, *val, *unit;
        int w, h;
        const lv_font_t *font;
        char last[24];
    } cell[LAYOUT_MAX_CELLS];
    datapage_click_cb_t click_cb;
    void *click_ctx;
};

/* (Re)builds the page inside parent at (x, y) with size w x h from cfg
 * (which must stay valid: it points into the app config). Existing objects
 * of dp are deleted first. */
void datapage_build(datapage_t *dp, lv_obj_t *parent, const page_cfg_t *cfg, int x, int y, int w, int h);
void datapage_delete(datapage_t *dp);
void datapage_refresh(datapage_t *dp);
/* Yellow frame around one cell (-1 = none), used by the field editor. */
void datapage_highlight(datapage_t *dp, int cell);
void datapage_set_click_cb(datapage_t *dp, datapage_click_cb_t cb, void *ctx);
