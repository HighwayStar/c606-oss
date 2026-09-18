/*
 * Settings menu, modelled on the reference device:
 *
 *   Settings
 *     Pages            -> Page 1..5 (layout name / off)
 *       Page n         -> Enable, Layout (picker with live preview), Fields
 *         Layout       -> page preview + up/down selector, tick = apply
 *         Fields       -> page preview, tap a cell (or move with keys) ->
 *           category   -> field list -> assigned, back to the preview
 *     Lap length       -> +/- screen (0.5 km steps, 0 = off)
 *     Backlight        -> +/- screen (10 % steps)
 *     Time zone        -> +/- screen (30 min steps)
 *     Theme            (tap: dark / light)
 *     Reset statistics
 *     System           -> USB storage, Power off, About
 *
 * Screens are stacked; each one is a full-screen object on the top layer
 * with a header (back arrow + title). List screens share one implementation
 * with key navigation; the layout picker and field editor are custom.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"

#include "board.h"
#include "menu.h"
#include "config.h"
#include "fields.h"
#include "layouts.h"
#include "datapage.h"
#include "stats.h"
#include "theme.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define MAX_DEPTH 8
#define MAX_ITEMS 16
#define HDR_H 28
#define ROW_H 40

/* the header stays dark in both themes (as on the reference device) */
static const lv_color_t C_HDR   = LV_COLOR_MAKE(0x18, 0x18, 0x18);
#define C_SEL    (theme_colors()->sel)
#define C_SEL_FG (theme_colors()->sel_fg)
#define C_ROW    (theme_colors()->panel)
#define C_FG     (theme_colors()->fg)
#define C_GREY   (theme_colors()->muted)

typedef struct screen screen_t;
struct screen {
    lv_obj_t *root, *back, *list;
    lv_obj_t *rows[MAX_ITEMS], *lbl[MAX_ITEMS], *right[MAX_ITEMS], *sw[MAX_ITEMS];
    int n, sel;
    void (*select_cb)(screen_t *s, int idx);   /* list screens */
    void (*key_cb)(screen_t *s, uint8_t key);  /* custom screens */
    void (*refresh_cb)(screen_t *s);           /* when a child screen is popped */
    int page, cell, cat;                       /* context */
};

static screen_t s_stack[MAX_DEPTH];
static int s_depth;
static void (*s_close_cb)(void);
static void (*s_action_cb[MENU_ACTION_COUNT])(void);
static lv_timer_t *s_timer;

/* previews */
static datapage_t s_dp_layout, s_dp_fields;
static page_cfg_t s_tmp_page;       /* layout picker edits a copy */
static lv_obj_t *s_layout_name;

static void pop(void);

/* ---- generic screen ---------------------------------------------------- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

static void back_cb(lv_event_t *e) { pop(); }

static screen_t *top(void) { return s_depth ? &s_stack[s_depth - 1] : NULL; }

static screen_t *push(const char *title)
{
    if (s_depth >= MAX_DEPTH) return NULL;
    screen_t *s = &s_stack[s_depth++];
    memset(s, 0, sizeof *s);
    s->sel = -1;

    s->root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s->root);
    lv_obj_set_size(s->root, LCD_H_RES, LCD_V_RES);
    lv_obj_add_style(s->root, &theme_st_bg, 0);
    lv_obj_set_style_bg_opa(s->root, LV_OPA_COVER, 0);

    lv_obj_t *hdr = lv_obj_create(s->root);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LCD_H_RES, HDR_H);
    lv_obj_set_style_bg_color(hdr, C_HDR, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_t *t = label(hdr, &lv_font_montserrat_14, lv_color_white(), title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);

    s->back = lv_obj_create(hdr);
    lv_obj_remove_style_all(s->back);
    lv_obj_set_size(s->back, 44, HDR_H);
    lv_obj_set_style_bg_opa(s->back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s->back, C_HDR, 0);
    lv_obj_add_event_cb(s->back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *a = label(s->back, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_LEFT);
    lv_obj_center(a);
    return s;
}

static void update_hl(screen_t *s)
{
    lv_obj_set_style_bg_color(s->back, s->sel == -1 ? C_SEL : C_HDR, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s->back, 0), s->sel == -1 ? C_SEL_FG : lv_color_white(), 0);
    for (int i = 0; i < s->n; i++) {
        bool on = i == s->sel;
        lv_obj_set_style_bg_color(s->rows[i], on ? C_SEL : C_ROW, 0);
        lv_obj_set_style_text_color(s->lbl[i], on ? C_SEL_FG : C_FG, 0);
        if (s->right[i]) lv_obj_set_style_text_color(s->right[i], on ? C_SEL_FG : C_GREY, 0);
        if (on && s->list) lv_obj_scroll_to_view(s->rows[i], LV_ANIM_OFF);
    }
}

static void row_cb(lv_event_t *e)
{
    screen_t *s = top();
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s || idx >= s->n) return;
    s->sel = idx;
    update_hl(s);
    if (s->select_cb) s->select_cb(s, idx);
}

static lv_obj_t *make_list(screen_t *s)
{
    s->list = lv_obj_create(s->root);
    lv_obj_remove_style_all(s->list);
    lv_obj_set_pos(s->list, 0, HDR_H);
    lv_obj_set_size(s->list, LCD_H_RES, LCD_V_RES - HDR_H);
    lv_obj_set_flex_flow(s->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s->list, LV_SCROLLBAR_MODE_AUTO);
    return s->list;
}

typedef enum { ITEM_PLAIN, ITEM_ARROW, ITEM_VALUE, ITEM_TOGGLE } item_kind_t;

static int add_item(screen_t *s, const char *text, item_kind_t kind, const char *right, bool on)
{
    if (s->n >= MAX_ITEMS) return -1;
    int i = s->n++;
    lv_obj_t *r = lv_obj_create(s->list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LCD_H_RES, ROW_H);
    lv_obj_add_style(r, &theme_st_panel, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    s->rows[i] = r;

    s->lbl[i] = label(r, &lv_font_montserrat_14, C_FG, text);
    lv_obj_align(s->lbl[i], LV_ALIGN_LEFT_MID, 10, 0);

    if (kind == ITEM_TOGGLE) {
        s->sw[i] = lv_switch_create(r);
        lv_obj_set_size(s->sw[i], 44, 22);
        lv_obj_align(s->sw[i], LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_clickable(s->sw[i], false);
        lv_obj_set_style_bg_color(s->sw[i], lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (on) lv_obj_add_state(s->sw[i], LV_STATE_CHECKED);
    } else if (kind == ITEM_ARROW || kind == ITEM_VALUE) {
        char buf[40];
        snprintf(buf, sizeof buf, "%s%s%s", right ? right : "", right && kind == ITEM_ARROW ? "  " : "",
                 kind == ITEM_ARROW ? LV_SYMBOL_RIGHT : "");
        s->right[i] = label(r, &lv_font_montserrat_14, C_GREY, buf);
        lv_obj_align(s->right[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }
    return i;
}

static void set_right(screen_t *s, int i, const char *right, bool arrow)
{
    if (!s->right[i]) return;
    char buf[40];
    snprintf(buf, sizeof buf, "%s%s%s", right, arrow ? "  " : "", arrow ? LV_SYMBOL_RIGHT : "");
    lv_label_set_text(s->right[i], buf);
}

static void set_toggle(screen_t *s, int i, bool on)
{
    if (!s->sw[i]) return;
    if (on) lv_obj_add_state(s->sw[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(s->sw[i], LV_STATE_CHECKED);
}

static void pop(void)
{
    if (!s_depth) return;
    screen_t *s = top();
    /* previews are children of the screen: forget them before the root goes */
    if (s_dp_layout.cont && lv_obj_get_parent(s_dp_layout.cont) == s->root) datapage_delete(&s_dp_layout);
    if (s_dp_fields.cont && lv_obj_get_parent(s_dp_fields.cont) == s->root) datapage_delete(&s_dp_fields);
    lv_obj_delete(s->root);
    memset(s, 0, sizeof *s);
    s_depth--;
    if (s_depth == 0) {
        menu_close();
    } else if (top()->refresh_cb) {
        top()->refresh_cb(top());
    }
}

/* ---- field chooser ----------------------------------------------------- */

static void fieldlist_select(screen_t *s, int idx)
{
    field_id_t ids[AGG_COUNT + 8];
    int n = field_category_items(s->cat, ids, sizeof ids / sizeof ids[0]);
    if (idx >= n) return;
    config_get()->page[s->page].field[s->cell] = ids[idx];
    config_save();
    pop();   /* field list */
    pop();   /* category list -> back at the field editor */
}

static void fieldlist_open(int page, int cell, int cat)
{
    screen_t *s = push(field_category_name(cat));
    if (!s) return;
    s->page = page; s->cell = cell; s->cat = cat;
    s->select_cb = fieldlist_select;
    make_list(s);
    field_id_t ids[AGG_COUNT + 8];
    int n = field_category_items(cat, ids, sizeof ids / sizeof ids[0]);
    field_id_t current = config_get()->page[page].field[cell];
    s->sel = 0;
    for (int i = 0; i < n; i++) {
        add_item(s, field_name(ids[i]), ITEM_PLAIN, NULL, false);
        if (ids[i] == current) s->sel = i;
    }
    update_hl(s);
}

static void category_select(screen_t *s, int idx)
{
    fieldlist_open(s->page, s->cell, idx);
}

static void category_open(int page, int cell)
{
    char title[24];
    snprintf(title, sizeof title, "Cell %d", cell + 1);
    screen_t *s = push(title);
    if (!s) return;
    s->page = page; s->cell = cell;
    s->select_cb = category_select;
    make_list(s);
    field_id_t current = config_get()->page[page].field[cell];
    s->sel = 0;
    for (int c = 0; c < field_category_count(); c++) {
        add_item(s, field_category_name(c), ITEM_ARROW, NULL, false);
        if (current >= FIELD_STAT_BASE && (current - FIELD_STAT_BASE) / AGG_COUNT == c) s->sel = c;
        if (current < FIELD_STAT_BASE && c == field_category_count() - 1) s->sel = c;
    }
    update_hl(s);
}

/* ---- field editor: page preview, tap/select a cell ----------------------- */

static void fields_cell_cb(datapage_t *dp, int cell, void *ctx)
{
    screen_t *s = top();
    s->sel = cell;
    datapage_highlight(dp, cell);
    update_hl(s);
    category_open(s->page, cell);
}

static void fields_key(screen_t *s, uint8_t key)
{
    if (key == 2 && s->sel > -1) s->sel--;
    if (key == 1 && s->sel < s_dp_fields.ncells - 1) s->sel++;
    if (key == 0) {
        if (s->sel < 0) { pop(); return; }
        category_open(s->page, s->sel);
        return;
    }
    datapage_highlight(&s_dp_fields, s->sel);
    update_hl(s);
}

static void fields_refresh(screen_t *s)
{
    /* a field may have changed: rebuild the preview */
    datapage_build(&s_dp_fields, s->root, &config_get()->page[s->page], 0, HDR_H, LCD_H_RES, LCD_V_RES - HDR_H);
    datapage_highlight(&s_dp_fields, s->sel);
}

static void fields_open(int page)
{
    char title[24];
    snprintf(title, sizeof title, "Page %d fields", page + 1);
    screen_t *s = push(title);
    if (!s) return;
    s->page = page;
    s->key_cb = fields_key;
    s->refresh_cb = fields_refresh;
    s->sel = 0;
    datapage_set_click_cb(&s_dp_fields, fields_cell_cb, NULL);
    fields_refresh(s);
    update_hl(s);
}

/* ---- layout picker ----------------------------------------------------- */

static void layout_preview(screen_t *s)
{
    datapage_build(&s_dp_layout, s->root, &s_tmp_page, 0, HDR_H, LCD_H_RES, LCD_V_RES - HDR_H);
    lv_obj_move_background(s_dp_layout.cont);   /* selector + tick stay on top */
    lv_label_set_text(s_layout_name, layout_get(s_tmp_page.layout)->name);
}

static void layout_step(screen_t *s, int dir)
{
    int n = layout_count();
    s_tmp_page.layout = (s_tmp_page.layout + n + dir) % n;
    layout_preview(s);
}

static void layout_apply(screen_t *s)
{
    config_get()->page[s->page] = s_tmp_page;
    config_save();
    pop();
}

static void layout_up_cb(lv_event_t *e)   { layout_step(top(), -1); }
static void layout_down_cb(lv_event_t *e) { layout_step(top(), +1); }
static void layout_ok_cb(lv_event_t *e)   { layout_apply(top()); }

static void layout_key(screen_t *s, uint8_t key)
{
    if (key == 2) layout_step(s, -1);
    else if (key == 1) layout_step(s, +1);
    else if (key == 0) layout_apply(s);
}

static void layout_open(int page)
{
    char title[24];
    snprintf(title, sizeof title, "Page %d layout", page + 1);
    screen_t *s = push(title);
    if (!s) return;
    s->page = page;
    s->key_cb = layout_key;
    s_tmp_page = config_get()->page[page];

    /* selector: up / name / down, like the reference device */
    lv_obj_t *sel = lv_obj_create(s->root);
    lv_obj_remove_style_all(sel);
    lv_obj_set_size(sel, 56, 120);
    lv_obj_align(sel, LV_ALIGN_RIGHT_MID, -8, 10);
    lv_obj_set_style_bg_color(sel, C_SEL, 0);
    lv_obj_set_style_bg_opa(sel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sel, 6, 0);
    lv_obj_t *up = lv_obj_create(sel);
    lv_obj_remove_style_all(up);
    lv_obj_set_size(up, 56, 40);
    lv_obj_align(up, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_event_cb(up, layout_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(up, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_UP));
    lv_obj_t *dn = lv_obj_create(sel);
    lv_obj_remove_style_all(dn);
    lv_obj_set_size(dn, 56, 40);
    lv_obj_align(dn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(dn, layout_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(dn, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_DOWN));
    s_layout_name = label(sel, &lv_font_montserrat_20, lv_color_white(), "");
    lv_obj_center(s_layout_name);

    lv_obj_t *ok = lv_button_create(s->root);
    lv_obj_set_size(ok, 48, 40);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_set_style_bg_color(ok, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_add_event_cb(ok, layout_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(ok, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_OK));

    layout_preview(s);
}

/* ---- page n ------------------------------------------------------------ */

static void page_refresh(screen_t *s)
{
    page_cfg_t *p = &config_get()->page[s->page];
    set_toggle(s, 0, p->enabled);
    set_right(s, 1, layout_get(p->layout)->name, true);
}

static void page_select(screen_t *s, int idx)
{
    page_cfg_t *p = &config_get()->page[s->page];
    switch (idx) {
    case 0:
        p->enabled = !p->enabled;
        config_save();
        page_refresh(s);
        break;
    case 1: layout_open(s->page); break;
    case 2: fields_open(s->page); break;
    default: break;
    }
}

static void page_open(int page)
{
    char title[24];
    snprintf(title, sizeof title, "Page %d", page + 1);
    screen_t *s = push(title);
    if (!s) return;
    s->page = page;
    s->select_cb = page_select;
    s->refresh_cb = page_refresh;
    make_list(s);
    page_cfg_t *p = &config_get()->page[page];
    add_item(s, "Enable", ITEM_TOGGLE, NULL, p->enabled);
    add_item(s, "Layout", ITEM_ARROW, layout_get(p->layout)->name, false);
    add_item(s, "Fields", ITEM_ARROW, NULL, false);
    s->sel = 0;
    update_hl(s);
}

/* ---- pages list -------------------------------------------------------- */

static void pages_refresh(screen_t *s)
{
    for (int i = 0; i < CFG_PAGES; i++) {
        page_cfg_t *p = &config_get()->page[i];
        set_right(s, i, p->enabled ? layout_get(p->layout)->name : "off", true);
    }
}

static void pages_select(screen_t *s, int idx)
{
    if (idx < CFG_PAGES) page_open(idx);
}

static void pages_open(void)
{
    screen_t *s = push("Pages");
    if (!s) return;
    s->select_cb = pages_select;
    s->refresh_cb = pages_refresh;
    make_list(s);
    for (int i = 0; i < CFG_PAGES; i++) {
        char t[16];
        snprintf(t, sizeof t, "Page %d", i + 1);
        add_item(s, t, ITEM_ARROW, "", false);
    }
    pages_refresh(s);
    s->sel = 0;
    update_hl(s);
}

/* ---- system ------------------------------------------------------------ */

static void about_open(void)
{
    screen_t *s = push("About");
    if (!s) return;
    const esp_app_desc_t *d = esp_app_get_description();
    lv_obj_t *l = label(s->root, &lv_font_montserrat_14, C_FG, "");
    lv_obj_add_style(l, &theme_st_text, 0);
    lv_label_set_text_fmt(l, "C606 open firmware\n%s\n\nESP-IDF %s\nbuilt %s\n\nheap %u KB  psram %u KB\nup %lu s",
                          d->version, d->idf_ver, d->date,
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          (unsigned long)(esp_timer_get_time() / 1000000));
    lv_obj_set_width(l, LCD_H_RES - 20);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 10, HDR_H + 10);
    update_hl(s);
}

static void system_select(screen_t *s, int idx)
{
    void (*cb)(void) = NULL;
    switch (idx) {
    case 0: cb = s_action_cb[MENU_ACTION_USB]; break;
    case 1: cb = s_action_cb[MENU_ACTION_POWER_OFF]; break;
    case 2: about_open(); return;
    default: return;
    }
    menu_close();
    if (cb) cb();
}

static void system_open(void)
{
    screen_t *s = push("System");
    if (!s) return;
    s->select_cb = system_select;
    make_list(s);
    add_item(s, LV_SYMBOL_USB "  USB storage", ITEM_PLAIN, NULL, false);
    add_item(s, LV_SYMBOL_POWER "  Power off", ITEM_PLAIN, NULL, false);
    add_item(s, "About", ITEM_ARROW, NULL, false);
    s->sel = 0;
    update_hl(s);
}

/* ---- +/- value screen -------------------------------------------------- */

typedef struct {
    void (*text)(char *buf, size_t n);   /* current value as text */
    void (*step)(int dir);               /* -1 / +1, saves the config */
} value_def_t;

static const value_def_t *s_value_def;
static lv_obj_t *s_value_lbl;

static void value_refresh(void)
{
    char buf[24];
    s_value_def->text(buf, sizeof buf);
    lv_label_set_text(s_value_lbl, buf);
}

static void value_step(int dir)
{
    s_value_def->step(dir);
    value_refresh();
}

static void value_minus_cb(lv_event_t *e) { value_step(-1); }
static void value_plus_cb(lv_event_t *e)  { value_step(+1); }

static void value_key(screen_t *s, uint8_t key)
{
    if (key == 2) value_step(+1);
    else if (key == 1) value_step(-1);
    else if (key == 0) pop();
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 64, 64);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, C_SEL, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(b, &lv_font_montserrat_28, C_SEL_FG, sym));
    return b;
}

static void value_open(const char *title, const value_def_t *def)
{
    screen_t *s = push(title);
    if (!s) return;
    s->key_cb = value_key;
    s_value_def = def;

    s_value_lbl = label(s->root, &lv_font_montserrat_28, C_FG, "");
    lv_obj_add_style(s_value_lbl, &theme_st_text, 0);
    lv_obj_align(s_value_lbl, LV_ALIGN_CENTER, 0, -30);

    lv_obj_align(round_button(s->root, LV_SYMBOL_MINUS, value_minus_cb), LV_ALIGN_CENTER, -60, 50);
    lv_obj_align(round_button(s->root, LV_SYMBOL_PLUS, value_plus_cb), LV_ALIGN_CENTER, 60, 50);

    lv_obj_t *h = label(s->root, &lv_font_montserrat_14, C_GREY, "key 2: +   key 1: -   key 0: back");
    lv_obj_add_style(h, &theme_st_muted, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -10);

    value_refresh();
    update_hl(s);
}

/* ---- settings root ----------------------------------------------------- */

static void lap_text(char *buf, size_t n)
{
    uint16_t m = config_get()->lap_len_m;
    if (m) snprintf(buf, n, "%u.%u km", m / 1000, m % 1000 / 100);
    else snprintf(buf, n, "off");
}

static void lap_step(int dir)
{
    int m = config_get()->lap_len_m + dir * CFG_LAP_STEP_M;
    if (m < 0) m = 0;
    if (m > CFG_LAP_MAX_M) m = CFG_LAP_MAX_M;
    config_get()->lap_len_m = m;
    config_save();
}

static void tz_text(char *buf, size_t n)
{
    int tz = config_get()->tz_min;
    snprintf(buf, n, "UTC%c%02d:%02d", tz < 0 ? '-' : '+', abs(tz) / 60, abs(tz) % 60);
}

static void tz_step(int dir)
{
    int tz = config_get()->tz_min + dir * 30;
    if (tz < -12 * 60) tz = -12 * 60;
    if (tz > 14 * 60) tz = 14 * 60;
    config_get()->tz_min = tz;
    config_save();
}

static void bl_text(char *buf, size_t n)
{
    snprintf(buf, n, "%u %%", config_get()->backlight);
}

static void bl_step(int dir)
{
    int v = config_get()->backlight + dir * 10;
    if (v < 10) v = 10;
    if (v > 100) v = 100;
    config_get()->backlight = v;
    config_save();
    if (s_action_cb[MENU_ACTION_BACKLIGHT]) s_action_cb[MENU_ACTION_BACKLIGHT]();
}

static const value_def_t k_lap_value = { lap_text, lap_step };
static const value_def_t k_tz_value  = { tz_text, tz_step };
static const value_def_t k_bl_value  = { bl_text, bl_step };


enum { ROOT_PAGES, ROOT_LAP, ROOT_BACKLIGHT, ROOT_TZ, ROOT_THEME, ROOT_RESET, ROOT_SYSTEM };

static void settings_select(screen_t *s, int idx)
{
    switch (idx) {
    case ROOT_PAGES:
        pages_open();
        break;
    case ROOT_LAP:
        value_open("Lap length", &k_lap_value);
        break;
    case ROOT_BACKLIGHT:
        value_open("Backlight", &k_bl_value);
        break;
    case ROOT_TZ:
        value_open("Time zone", &k_tz_value);
        break;
    case ROOT_THEME:
        config_get()->theme = config_get()->theme == THEME_LIGHT ? THEME_DARK : THEME_LIGHT;
        config_save();
        theme_set(config_get()->theme);
        set_right(s, idx, theme_name(config_get()->theme), false);
        update_hl(s);   /* rows keep local colours: re-apply for the new theme */
        break;
    case ROOT_RESET:
        stats_reset();
        menu_close();
        break;
    case ROOT_SYSTEM:
        system_open();
        break;
    default:
        break;
    }
}

static void settings_refresh(screen_t *s)
{
    char buf[16];
    lap_text(buf, sizeof buf);
    set_right(s, ROOT_LAP, buf, true);
    bl_text(buf, sizeof buf);
    set_right(s, ROOT_BACKLIGHT, buf, true);
    tz_text(buf, sizeof buf);
    set_right(s, ROOT_TZ, buf, true);
}

static void timer_cb(lv_timer_t *t)
{
    if (s_dp_layout.cont) datapage_refresh(&s_dp_layout);
    if (s_dp_fields.cont) datapage_refresh(&s_dp_fields);
}

void menu_open(void)
{
    if (s_depth) return;
    screen_t *s = push("Settings");
    if (!s) return;
    s->select_cb = settings_select;
    s->refresh_cb = settings_refresh;
    make_list(s);
    char buf[16];
    add_item(s, "Pages", ITEM_ARROW, NULL, false);
    lap_text(buf, sizeof buf);
    add_item(s, "Lap length", ITEM_ARROW, buf, false);
    bl_text(buf, sizeof buf);
    add_item(s, "Backlight", ITEM_ARROW, buf, false);
    tz_text(buf, sizeof buf);
    add_item(s, "Time zone", ITEM_ARROW, buf, false);
    add_item(s, "Theme", ITEM_VALUE, theme_name(config_get()->theme), false);
    add_item(s, "Reset statistics", ITEM_PLAIN, NULL, false);
    add_item(s, "System", ITEM_ARROW, NULL, false);
    s->sel = 0;
    update_hl(s);
    s_timer = lv_timer_create(timer_cb, 500, NULL);
}

void menu_close(void)
{
    datapage_delete(&s_dp_layout);
    datapage_delete(&s_dp_fields);
    while (s_depth) {
        lv_obj_delete(s_stack[--s_depth].root);
    }
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_close_cb) s_close_cb();
}

bool menu_active(void) { return s_depth > 0; }

void menu_set_close_cb(void (*cb)(void)) { s_close_cb = cb; }

void menu_set_action_cb(menu_action_t a, void (*cb)(void))
{
    if (a < MENU_ACTION_COUNT) s_action_cb[a] = cb;
}

bool menu_key(uint8_t key, uint8_t evt)
{
    screen_t *s = top();
    if (!s) return false;
    if (evt != KEY_EVT_CLICK) return true;   /* holds are handled by main.c before we see them */
    if (s->key_cb) {
        s->key_cb(s, key);
        return true;
    }
    if (key == 2 && s->sel > -1) s->sel--;
    else if (key == 1 && s->sel < s->n - 1) s->sel++;
    else if (key == 0) {
        if (s->sel < 0) pop();
        else if (s->select_cb) s->select_cb(s, s->sel);
        return true;
    }
    update_hl(s);
    return true;
}
