#include <string.h>
#include "datapage.h"
#include "fields.h"
#include "theme.h"

#define C_HL (theme_colors()->sel)

#define NAME_H 16

static void cell_click_cb(lv_event_t *e)
{
    datapage_t *dp = lv_event_get_user_data(e);
    lv_obj_t *box = lv_event_get_target(e);
    for (int i = 0; i < dp->ncells; i++) {
        if (dp->cell[i].box == box && dp->click_cb) {
            dp->click_cb(dp, i, dp->click_ctx);
            return;
        }
    }
}

/* Largest font whose digits fit the cell: height first, then width for
 * `len` characters plus the unit. Montserrat digits are ~0.62 em wide. */
static const lv_font_t *pick_font(int w, int h, int len, int unit_len)
{
    static const lv_font_t *fonts[] = { &lv_font_montserrat_48, &lv_font_montserrat_28,
                                        &lv_font_montserrat_20, &lv_font_montserrat_14 };
    static const int sizes[] = { 48, 28, 20, 14 };
    int avail_h = h - NAME_H - 2;
    int avail_w = w - 6 - (unit_len ? unit_len * 9 + 2 : 0);
    for (int i = 0; i < 4; i++) {
        if (sizes[i] + 6 <= avail_h && len * sizes[i] * 62 / 100 <= avail_w) return fonts[i];
    }
    return &lv_font_montserrat_14;
}

void datapage_delete(datapage_t *dp)
{
    if (dp->cont) lv_obj_delete(dp->cont);
    memset(dp, 0, sizeof *dp);
    dp->hl = -1;
}

void datapage_build(datapage_t *dp, lv_obj_t *parent, const page_cfg_t *cfg, int x, int y, int w, int h)
{
    datapage_click_cb_t cb = dp->click_cb;
    void *ctx = dp->click_ctx;
    datapage_delete(dp);
    dp->click_cb = cb;
    dp->click_ctx = ctx;
    dp->cfg = cfg;
    dp->w = w;
    dp->h = h;

    const layout_t *l = layout_get(cfg->layout);
    dp->ncells = layout_cells(l);

    dp->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(dp->cont);
    lv_obj_set_pos(dp->cont, x, y);
    lv_obj_set_size(dp->cont, w, h);

    for (int i = 0; i < dp->ncells; i++) {
        int cx, cy, cw, ch;
        layout_cell_rect(l, i, w, h, &cx, &cy, &cw, &ch);
        dp->cell[i].w = cw;
        dp->cell[i].h = ch;

        lv_obj_t *box = lv_obj_create(dp->cont);
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, cx, cy);
        lv_obj_set_size(box, cw, ch);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_add_style(box, &theme_st_line, 0);
        lv_obj_add_style(box, &theme_st_bg, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(box, cell_click_cb, LV_EVENT_CLICKED, dp);
        dp->cell[i].box = box;

        field_id_t f = cfg->field[i];
        lv_obj_t *n = lv_label_create(box);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
        lv_obj_add_style(n, &theme_st_muted, 0);
        if (f == FIELD_NONE) lv_obj_set_style_text_opa(n, LV_OPA_50, 0);
        lv_label_set_text(n, field_name(f));
        lv_label_set_long_mode(n, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(n, cw - 4);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 1);
        dp->cell[i].name = n;

        /* value + unit side by side, centred, bottom-aligned */
        lv_obj_t *row = lv_obj_create(box);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, NAME_H);
        lv_obj_set_size(row, cw, ch - NAME_H);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 2, 0);
        lv_obj_set_clickable(row, false);

        lv_obj_t *v = lv_label_create(row);
        lv_obj_add_style(v, &theme_st_text, 0);
        lv_label_set_text(v, "");
        dp->cell[i].val = v;

        lv_obj_t *u = lv_label_create(row);
        lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
        lv_obj_add_style(u, &theme_st_muted, 0);
        lv_label_set_text(u, field_unit(f));
        lv_obj_set_style_translate_y(u, 3, 0);   /* sit on the digits' baseline */
        dp->cell[i].unit = u;

        dp->cell[i].last[0] = 0;
        dp->cell[i].font = NULL;
    }
    datapage_refresh(dp);
}

void datapage_refresh(datapage_t *dp)
{
    char buf[24];
    for (int i = 0; i < dp->ncells; i++) {
        field_id_t f = dp->cfg->field[i];
        field_value(f, buf, sizeof buf);
        if (strcmp(buf, dp->cell[i].last) == 0) continue;
        strcpy(dp->cell[i].last, buf);
        const lv_font_t *font = pick_font(dp->cell[i].w, dp->cell[i].h, strlen(buf), strlen(field_unit(f)));
        if (font != dp->cell[i].font) {
            dp->cell[i].font = font;
            lv_obj_set_style_text_font(dp->cell[i].val, font, 0);
        }
        lv_label_set_text(dp->cell[i].val, buf);
    }
}

void datapage_highlight(datapage_t *dp, int cell)
{
    for (int i = 0; i < dp->ncells; i++) {
        bool on = i == cell;
        lv_obj_set_style_border_width(dp->cell[i].box, on ? 3 : 1, 0);
        if (on) lv_obj_set_style_border_color(dp->cell[i].box, C_HL, 0);
        else lv_obj_remove_local_style_prop(dp->cell[i].box, LV_STYLE_BORDER_COLOR, 0);
    }
    dp->hl = cell;
}

void datapage_set_click_cb(datapage_t *dp, datapage_click_cb_t cb, void *ctx)
{
    dp->click_cb = cb;
    dp->click_ctx = ctx;
}
