/*
 * Data field widget: current value + session min / max / avg of one
 * measured parameter (see stats.c).
 *
 *  half (116x84)                     wide (232x84)
 *  +------------------------+        +---------------------------------------+
 *  | Speed            km/h  |        | Speed                          km/h   |
 *  |         23.4           |        |                        min      0.0   |
 *  |  min     max     avg   |        |  23.4                  max     45.2   |
 *  |  0.0    45.2    18.3   |        |                        avg     18.3   |
 *  +------------------------+        +---------------------------------------+
 */
#include <string.h>
#include "widget.h"

static const lv_color_t C_BOX     = LV_COLOR_MAKE(0x18, 0x18, 0x18);
static const lv_color_t C_CAPTION = LV_COLOR_MAKE(0x90, 0x90, 0x90);
static const lv_color_t C_STALE   = LV_COLOR_MAKE(0x70, 0x70, 0x70);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

void widget_create(widget_t *w, lv_obj_t *parent, stat_id_t id, uint8_t cols, uint8_t col, uint8_t row, int y0)
{
    const stat_info_t *info = stats_info(id);
    memset(w, 0, sizeof *w);
    w->id = id;
    w->cols = cols == 2 ? 2 : 1;
    int width = w->cols * WIDGET_COL_W + (w->cols - 1) * WIDGET_GAP;

    w->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(w->cont);
    lv_obj_set_size(w->cont, width, WIDGET_ROW_H);
    lv_obj_set_pos(w->cont, WIDGET_GAP + col * (WIDGET_COL_W + WIDGET_GAP), y0 + row * (WIDGET_ROW_H + WIDGET_GAP));
    lv_obj_set_style_bg_color(w->cont, C_BOX, 0);
    lv_obj_set_style_bg_opa(w->cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(w->cont, 6, 0);
    lv_obj_set_clickable(w->cont, false);

    lv_obj_t *t = label(w->cont, &lv_font_montserrat_14, C_CAPTION, info->name);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_t *u = label(w->cont, &lv_font_montserrat_14, C_CAPTION, info->unit);
    lv_obj_align(u, LV_ALIGN_TOP_RIGHT, -4, 2);

    static const char *cap[3] = { "min", "max", "avg" };
    lv_obj_t **val[3] = { &w->min, &w->max, &w->avg };

    if (w->cols == 1) {
        w->cur = label(w->cont, &lv_font_montserrat_28, lv_color_white(), "--");
        lv_obj_align(w->cur, LV_ALIGN_TOP_MID, 0, 16);
        for (int i = 0; i < 3; i++) {
            int dx = (i - 1) * 39;
            lv_obj_t *c = label(w->cont, &lv_font_unscii_8, C_CAPTION, cap[i]);
            lv_obj_align(c, LV_ALIGN_TOP_MID, dx, 52);
            *val[i] = label(w->cont, &lv_font_montserrat_14, lv_color_white(), "--");
            lv_obj_align(*val[i], LV_ALIGN_TOP_MID, dx, 62);
        }
    } else {
        w->cur = label(w->cont, &lv_font_montserrat_48, lv_color_white(), "--");
        lv_obj_align(w->cur, LV_ALIGN_TOP_LEFT, 6, 22);
        for (int i = 0; i < 3; i++) {
            int y = 20 + i * 18;
            lv_obj_t *c = label(w->cont, &lv_font_unscii_8, C_CAPTION, cap[i]);
            lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -60, y + 4);
            *val[i] = label(w->cont, &lv_font_montserrat_14, lv_color_white(), "--");
            lv_obj_align(*val[i], LV_ALIGN_TOP_RIGHT, -4, y);
        }
    }
}

static void set_if_changed(lv_obj_t *l, char *last, const char *txt)
{
    if (strcmp(last, txt) != 0) {
        strncpy(last, txt, 15);
        last[15] = 0;
        lv_label_set_text(l, txt);
    }
}

void widget_refresh(widget_t *w)
{
    stat_values_t v;
    char buf[16];
    stats_get(w->id, &v);

    stats_format(w->id, v.cur, v.valid && v.live, buf, sizeof buf);
    set_if_changed(w->cur, w->last[0], buf);
    lv_obj_set_style_text_color(w->cur, v.live ? lv_color_white() : C_STALE, 0);

    stats_format(w->id, v.min, v.valid, buf, sizeof buf);
    set_if_changed(w->min, w->last[1], buf);
    stats_format(w->id, v.max, v.valid, buf, sizeof buf);
    set_if_changed(w->max, w->last[2], buf);
    stats_format(w->id, v.avg, v.valid, buf, sizeof buf);
    set_if_changed(w->avg, w->last[3], buf);
}
