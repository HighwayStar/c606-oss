/*
 * Settings menu, modelled on the reference device:
 *
 *   Settings
 *     Pages            -> Page 1..5 (layout name / off)
 *     Sensors          -> wheel circumference, known sensors (forget), add (ANT scan)
 *     Profile          -> weight, height, birth year, sex, max HR, LTHR, FTP, bike weight,
 *                         HR zone mode (% max HR / % LTHR); Health: BMI, BMR,
 *                         the HR and power zone tables (health.c)
 *       Page n         -> Enable, Layout (picker with live preview), Fields
 *         Layout       -> page preview + up/down selector, tick = apply
 *         Fields       -> page preview, tap a cell (or move with keys) ->
 *           category   -> field list -> assigned, back to the preview
 *     Lap length       -> +/- screen (0.5 km steps, 0 = off)
 *     Auto pause       (toggle: pause when standing still)
 *     Backlight        -> +/- screen (10 % steps)
 *     Time zone        -> +/- screen (30 min steps)
 *     Theme            (tap: dark / light)
 *     Auto theme       (toggle: light by day, dark after sunset)
 *     Map layers       (toggle per road class / water / coastline)
 *     Route            -> GPX file drawn on the map (c606oss/routes/, .gpx), or none
 *       file           -> preview: track outline, length, climb / descent,
 *                         Reverse toggle, "Use this route"
 *     History          -> recorded rides (ours and the vendor's), newest first
 *       ride           -> track outline + summary (time, distance, speed, HR,
 *                         cadence, power, calories, climb, laps),
 *                         "Use as route" (the ride becomes the map's route), Delete
 *     Reset statistics
 *     System           -> USB storage, Power off, Reset settings (confirm), About
 *
 * Screens are stacked; each one is a full-screen object on the top layer
 * with a header (back arrow + title). List screens share one implementation
 * with key navigation; the layout picker and field editor are custom.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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
#include "sun.h"
#include "mapview.h"
#include "route.h"
#include "history.h"
#include "ant.h"
#include "health.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define MAX_DEPTH 8
#define MAX_ITEMS 40
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
    void (*refresh_cb)(screen_t *s);           /* when a child screen is popped (and 2x/s if live) */
    void (*close_cb)(screen_t *s);             /* before the screen is deleted */
    bool live;
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

static void list_clear(screen_t *s)
{
    lv_obj_clean(s->list);
    s->n = 0;
    memset(s->rows, 0, sizeof s->rows);
    memset(s->lbl, 0, sizeof s->lbl);
    memset(s->right, 0, sizeof s->right);
    memset(s->sw, 0, sizeof s->sw);
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
        /* a long name (route file, sensor) ends in "..." before the value */
        lv_obj_update_layout(s->right[i]);
        lv_obj_set_width(s->lbl[i], LCD_H_RES - 28 - lv_obj_get_width(s->right[i]));
        lv_label_set_long_mode(s->lbl[i], LV_LABEL_LONG_DOT);
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
    if (s->close_cb) s->close_cb(s);
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
    config_page(config_get(), s->page)->field[s->cell] = ids[idx];
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
    field_id_t current = config_page(config_get(), page)->field[cell];
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
    field_id_t current = config_page(config_get(), page)->field[cell];
    s->sel = 0;
    for (int c = 0; c < field_category_count(); c++) {
        add_item(s, field_category_name(c), ITEM_ARROW, NULL, false);
        field_id_t ids[AGG_COUNT + 8];
        int n = field_category_items(c, ids, sizeof ids / sizeof ids[0]);
        for (int i = 0; i < n; i++) {
            if (ids[i] == current) s->sel = c;
        }
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
    if (key == KEY_MENU_UP && s->sel > -1) s->sel--;
    if (key == KEY_MENU_DOWN && s->sel < s_dp_fields.ncells - 1) s->sel++;
    if (key == KEY_MENU_BACK) { pop(); return; }
    if (key == KEY_MENU_SELECT) {
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
    datapage_build(&s_dp_fields, s->root, config_page(config_get(), s->page), 0, HDR_H, LCD_H_RES, LCD_V_RES - HDR_H);
    if (s_dp_fields.ncells == 0) s->sel = -1;   /* "Map" layout: nothing to edit, select goes back */
    datapage_highlight(&s_dp_fields, s->sel);
}

static void page_title(char *buf, size_t n, int page, const char *suffix)
{
    if (page == CFG_PAGES) snprintf(buf, n, "Map%s", suffix);
    else snprintf(buf, n, "Page %d%s", page + 1, suffix);
}

static void fields_open(int page)
{
    char title[24];
    page_title(title, sizeof title, page, " fields");
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
    s_tmp_page.layout = layout_next(s_tmp_page.layout, dir, s->page == CFG_PAGES);
    layout_preview(s);
}

static void layout_apply(screen_t *s)
{
    *config_page(config_get(), s->page) = s_tmp_page;
    config_save();
    pop();
}

static void layout_up_cb(lv_event_t *e)   { layout_step(top(), -1); }
static void layout_down_cb(lv_event_t *e) { layout_step(top(), +1); }
static void layout_ok_cb(lv_event_t *e)   { layout_apply(top()); }

static void layout_key(screen_t *s, uint8_t key)
{
    if (key == KEY_MENU_UP) layout_step(s, -1);
    else if (key == KEY_MENU_DOWN) layout_step(s, +1);
    else if (key == KEY_MENU_SELECT) layout_apply(s);
    else if (key == KEY_MENU_BACK) pop();
}

static void layout_open(int page)
{
    char title[24];
    page_title(title, sizeof title, page, " layout");
    screen_t *s = push(title);
    if (!s) return;
    s->page = page;
    s->key_cb = layout_key;
    s_tmp_page = *config_page(config_get(), page);

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
    page_cfg_t *p = config_page(config_get(), s->page);
    set_toggle(s, 0, p->enabled);
    set_right(s, 1, layout_get(p->layout)->name, true);
}

static void page_select(screen_t *s, int idx)
{
    page_cfg_t *p = config_page(config_get(), s->page);
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
    page_title(title, sizeof title, page, "");
    screen_t *s = push(title);
    if (!s) return;
    s->page = page;
    s->select_cb = page_select;
    s->refresh_cb = page_refresh;
    make_list(s);
    page_cfg_t *p = config_page(config_get(), page);
    add_item(s, "Enable", ITEM_TOGGLE, NULL, p->enabled);
    add_item(s, "Layout", ITEM_ARROW, layout_get(p->layout)->name, false);
    add_item(s, "Fields", ITEM_ARROW, NULL, false);
    s->sel = 0;
    update_hl(s);
}

/* ---- pages list -------------------------------------------------------- */

static void pages_refresh(screen_t *s)
{
    for (int i = 0; i <= CFG_PAGES; i++) {
        page_cfg_t *p = config_page(config_get(), i);
        set_right(s, i, p->enabled ? layout_get(p->layout)->name : "off", true);
    }
}

static void pages_select(screen_t *s, int idx)
{
    if (idx <= CFG_PAGES) page_open(idx);
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
    add_item(s, "Map", ITEM_ARROW, "", false);
    pages_refresh(s);
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
    if (key == KEY_MENU_UP) value_step(+1);
    else if (key == KEY_MENU_DOWN) value_step(-1);
    else if (key == KEY_MENU_SELECT || key == KEY_MENU_BACK) pop();
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 64, 64);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, C_SEL, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);   /* hold to run */
    lv_obj_center(label(b, &lv_font_montserrat_28, C_SEL_FG, sym));
    return b;
}

static void value_close(screen_t *s) { config_save(); }

static void value_open(const char *title, const value_def_t *def)
{
    screen_t *s = push(title);
    if (!s) return;
    s->key_cb = value_key;
    s->close_cb = value_close;   /* one save per visit, not per step */
    s_value_def = def;

    s_value_lbl = label(s->root, &lv_font_montserrat_28, C_FG, "");
    lv_obj_add_style(s_value_lbl, &theme_st_text, 0);
    lv_obj_align(s_value_lbl, LV_ALIGN_CENTER, 0, -30);

    lv_obj_align(round_button(s->root, LV_SYMBOL_MINUS, value_minus_cb), LV_ALIGN_CENTER, -60, 50);
    lv_obj_align(round_button(s->root, LV_SYMBOL_PLUS, value_plus_cb), LV_ALIGN_CENTER, 60, 50);

    lv_obj_t *h = label(s->root, &lv_font_montserrat_14, C_GREY,
                        "key " KEY_STR(KEY_MENU_UP) ": +   key " KEY_STR(KEY_MENU_DOWN) ": -   key " KEY_STR(KEY_MENU_SELECT) ": back");
    lv_obj_add_style(h, &theme_st_muted, 0);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -10);

    value_refresh();
    update_hl(s);
}

/* ---- value definitions ------------------------------------------------- */

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
    if (s_action_cb[MENU_ACTION_BACKLIGHT]) s_action_cb[MENU_ACTION_BACKLIGHT]();
}

static void wheel_text(char *buf, size_t n)
{
    snprintf(buf, n, "%u mm", config_get()->wheel_mm);
}

static void wheel_step(int dir)
{
    int v = config_get()->wheel_mm + dir;
    if (v < CFG_WHEEL_MIN_MM) v = CFG_WHEEL_MIN_MM;
    if (v > CFG_WHEEL_MAX_MM) v = CFG_WHEEL_MAX_MM;
    config_get()->wheel_mm = v;
    ant_set_wheel_mm(v);
}

/* rider profile */
static void weight_text(char *buf, size_t n) { snprintf(buf, n, "%u kg", config_get()->weight_kg); }
static void weight_step(int dir)
{
    int v = config_get()->weight_kg + dir;
    if (v < CFG_WEIGHT_MIN_KG) v = CFG_WEIGHT_MIN_KG;
    if (v > CFG_WEIGHT_MAX_KG) v = CFG_WEIGHT_MAX_KG;
    config_get()->weight_kg = v;
}
static void height_text(char *buf, size_t n) { snprintf(buf, n, "%u cm", config_get()->height_cm); }
static void height_step(int dir)
{
    int v = config_get()->height_cm + dir;
    if (v < CFG_HEIGHT_MIN_CM) v = CFG_HEIGHT_MIN_CM;
    if (v > CFG_HEIGHT_MAX_CM) v = CFG_HEIGHT_MAX_CM;
    config_get()->height_cm = v;
}
static void birth_text(char *buf, size_t n)
{
    int age = health_age();
    if (age) snprintf(buf, n, "%u (%d y)", config_get()->birth_year, age);
    else snprintf(buf, n, "%u", config_get()->birth_year);
}
static void birth_step(int dir)
{
    int v = config_get()->birth_year + dir;
    if (v < CFG_BIRTH_MIN) v = CFG_BIRTH_MIN;
    if (v > CFG_BIRTH_MAX) v = CFG_BIRTH_MAX;
    config_get()->birth_year = v;
}
/* max HR and LTHR: "auto" (0, the estimate) below the manual range; + from
 * auto starts at the estimate so the user only has to correct it */
static void hr_text(char *buf, size_t n, uint8_t cfg, int est)
{
    if (cfg) snprintf(buf, n, "%u bpm", cfg);
    else snprintf(buf, n, "auto (%d)", est);
}
static uint8_t hr_step(uint8_t cfg, int est, int dir)
{
    int v;
    if (!cfg) v = dir > 0 ? est : 0;
    else v = cfg + dir;
    if (v < CFG_HR_MIN) v = 0;
    if (v > CFG_HR_MAX) v = CFG_HR_MAX;
    return v;
}
static void maxhr_text(char *buf, size_t n) { hr_text(buf, n, config_get()->max_hr, health_max_hr()); }
static void maxhr_step(int dir) { config_get()->max_hr = hr_step(config_get()->max_hr, health_max_hr(), dir); }
static void lthr_text(char *buf, size_t n) { hr_text(buf, n, config_get()->lthr, health_lthr()); }
static void lthr_step(int dir) { config_get()->lthr = hr_step(config_get()->lthr, health_lthr(), dir); }
static void ftp_text(char *buf, size_t n)
{
    if (config_get()->ftp_w) snprintf(buf, n, "%u W", config_get()->ftp_w);
    else snprintf(buf, n, "off");
}
static void ftp_step(int dir)
{
    int v = config_get()->ftp_w + dir * 5;
    if (v < 0) v = 0;
    if (v > CFG_FTP_MAX_W) v = CFG_FTP_MAX_W;
    config_get()->ftp_w = v;
}

static void bike_text(char *buf, size_t n) { snprintf(buf, n, "%u.%u kg", config_get()->bike_kg10 / 10, config_get()->bike_kg10 % 10); }
static void bike_step(int dir)
{
    int v = config_get()->bike_kg10 + dir;
    if (v < 0) v = 0;
    if (v > CFG_BIKE_MAX_KG10) v = CFG_BIKE_MAX_KG10;
    config_get()->bike_kg10 = v;
}
static const value_def_t k_bike_value   = { bike_text, bike_step };
static const value_def_t k_weight_value = { weight_text, weight_step };
static const value_def_t k_height_value = { height_text, height_step };
static const value_def_t k_birth_value  = { birth_text, birth_step };
static const value_def_t k_maxhr_value  = { maxhr_text, maxhr_step };
static const value_def_t k_lthr_value   = { lthr_text, lthr_step };
static const value_def_t k_ftp_value    = { ftp_text, ftp_step };

static const value_def_t k_wheel_value = { wheel_text, wheel_step };
static const value_def_t k_lap_value = { lap_text, lap_step };
static const value_def_t k_tz_value  = { tz_text, tz_step };
static const value_def_t k_bl_value  = { bl_text, bl_step };


/* ---- sensors ----------------------------------------------------------- */

static void sensor_name(const cfg_sensor_t *e, char *buf, size_t n)
{
    snprintf(buf, n, "%s %u", ant_dev_name(e->dev_type), e->dev_num);
}

static const char *sensor_status(const cfg_sensor_t *e)
{
    size_t nch;
    const ant_channel_t *ch = ant_channels(&nch);
    for (size_t i = 0; i < nch; i++) {
        if (ch[i].dev_type != e->dev_type) continue;
        if (ant_live(e->dev_type, e->dev_type)) return "ok";
        return ch[i].state == ANT_ST_SEARCHING || ch[i].state == ANT_ST_TIMEOUT ? "searching" : "--";
    }
    return "--";
}

/* scan results arrive in the nRF task; the screen picks them up from its
 * 2 Hz refresh */
static ant_scan_result_t s_scan_res[CFG_MAX_SENSORS];
static volatile int s_scan_n, s_scan_shown;
static volatile bool s_scan_done;

static void scan_cb(const ant_scan_result_t *r, bool end, void *ctx)
{
    if (end) {
        s_scan_done = true;
        return;
    }
    for (int i = 0; i < s_scan_n; i++) {
        if (s_scan_res[i].dev_type == r->dev_type && s_scan_res[i].dev_num == r->dev_num) {
            s_scan_res[i].rssi = r->rssi;
            return;
        }
    }
    if (s_scan_n < CFG_MAX_SENSORS) s_scan_res[s_scan_n++] = *r;
}

static void scan_refresh(screen_t *s)
{
    int n = s_scan_n;
    if (n == s_scan_shown && s->n) {
        if (s_scan_done && s->lbl[0]) return;
    }
    s_scan_shown = n;
    list_clear(s);
    char t[40];
    snprintf(t, sizeof t, "%s  %d found", s_scan_done ? "Scan finished:" : "Scanning...", n);
    add_item(s, t, ITEM_PLAIN, NULL, false);
    lv_obj_set_style_text_color(s->lbl[0], C_GREY, 0);
    for (int i = 0; i < n; i++) {
        char name[32], rssi[16];
        cfg_sensor_t e = { .dev_type = s_scan_res[i].dev_type, .dev_num = s_scan_res[i].dev_num };
        sensor_name(&e, name, sizeof name);
        snprintf(rssi, sizeof rssi, "%d dBm", s_scan_res[i].rssi);
        add_item(s, name, ITEM_VALUE, rssi, false);
    }
    if (s->sel >= s->n) s->sel = s->n - 1;
    if (s->sel < 1 && n) s->sel = 1;
    update_hl(s);
}

static void scan_select(screen_t *s, int idx)
{
    if (idx < 1 || idx - 1 >= s_scan_n) return;
    const ant_scan_result_t *r = &s_scan_res[idx - 1];
    ant_scan(0);
    if (config_sensor_add(r->dev_type, r->dev_num, r->trans_type)) {
        ant_connect(r->dev_type, r->dev_num, r->trans_type);
    }
    pop();   /* back to the sensor list */
}

static void scan_close(screen_t *s)
{
    if (!s_scan_done) ant_scan(0);
}

static void scan_open(void)
{
    screen_t *s = push("Add sensor");
    if (!s) return;
    s->select_cb = scan_select;
    s->refresh_cb = scan_refresh;
    s->close_cb = scan_close;
    s->live = true;
    make_list(s);
    s_scan_n = 0;
    s_scan_shown = -1;
    s_scan_done = false;
    ant_set_scan_cb(scan_cb, NULL);
    ant_scan(30);
    s->sel = -1;
    scan_refresh(s);
}

static void sensor_select(screen_t *s, int idx)
{
    if (idx != 1) { pop(); return; }
    int i = s->page;   /* index into the config list */
    if (i < config_get()->nsensors) {
        ant_forget(config_get()->sensors[i].dev_type);
        config_sensor_remove(i);
    }
    pop();
}

static void sensor_open(int idx)
{
    const cfg_sensor_t *e = &config_get()->sensors[idx];
    char name[32];
    sensor_name(e, name, sizeof name);
    screen_t *s = push(name);
    if (!s) return;
    s->page = idx;
    s->select_cb = sensor_select;
    make_list(s);
    lv_obj_t *h = label(s->root, &lv_font_montserrat_14, C_GREY, "");
    lv_obj_add_style(h, &theme_st_muted, 0);
    lv_label_set_text_fmt(h, "ANT+ %s\ndevice number %u\ntransmission type %u\nstate: %s",
                          ant_dev_name(e->dev_type), e->dev_num, e->trans_type, sensor_status(e));
    lv_obj_set_width(h, LCD_H_RES - 16);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, HDR_H + 2 * ROW_H + 16);
    add_item(s, "Back", ITEM_PLAIN, NULL, false);
    add_item(s, LV_SYMBOL_TRASH "  Forget sensor", ITEM_PLAIN, NULL, false);
    s->sel = 0;
    update_hl(s);
}

static void sensors_refresh(screen_t *s)
{
    const app_cfg_t *c = config_get();
    if (s->n != c->nsensors + 2) {
        list_clear(s);
        char buf[32];
        wheel_text(buf, sizeof buf);
        add_item(s, "Wheel", ITEM_ARROW, buf, false);
        for (int i = 0; i < c->nsensors; i++) {
            sensor_name(&c->sensors[i], buf, sizeof buf);
            add_item(s, buf, ITEM_ARROW, "", false);
        }
        add_item(s, LV_SYMBOL_PLUS "  Add sensor", ITEM_PLAIN, NULL, false);
        if (s->sel >= s->n) s->sel = s->n - 1;
    }
    char buf[32];
    wheel_text(buf, sizeof buf);
    set_right(s, 0, buf, true);
    for (int i = 0; i < c->nsensors; i++) {
        set_right(s, 1 + i, sensor_status(&c->sensors[i]), true);
    }
    update_hl(s);
}

static void sensors_select(screen_t *s, int idx)
{
    int n = config_get()->nsensors;
    if (idx == 0) value_open("Wheel circumference", &k_wheel_value);
    else if (idx <= n) sensor_open(idx - 1);
    else scan_open();
}

static void sensors_open(void)
{
    screen_t *s = push("Sensors");
    if (!s) return;
    s->select_cb = sensors_select;
    s->refresh_cb = sensors_refresh;
    s->live = true;
    make_list(s);
    s->sel = 0;
    sensors_refresh(s);
}

/* ---- profile / health ---------------------------------------------------- */

/* Derived values and the zone tables: what the data fields and the FIT
 * file's time-in-zone arrays are based on. */
static void health_open(void)
{
    static const char *k_hr_names[HR_ZONES] = { "recovery", "endurance", "tempo", "threshold", "maximum" };
    static const char *k_pwr_names[PWR_ZONES] = { "recovery", "endurance", "tempo", "threshold", "VO2max", "anaerobic", "neuromusc." };
    screen_t *s = push("Health");
    if (!s) return;
    lv_obj_t *box = lv_obj_create(s->root);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 0, HDR_H);
    lv_obj_set_size(box, LCD_H_RES, LCD_V_RES - HDR_H);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);

    char txt[640], line[64];
    int n = 0;
    const app_cfg_t *c = config_get();
    float bmi = health_bmi();
    n += snprintf(txt + n, sizeof txt - n, "%s, %u kg, %u cm", c->sex ? "Female" : "Male", c->weight_kg, c->height_cm);
    if (health_age()) n += snprintf(txt + n, sizeof txt - n, ", %d y", health_age());
    n += snprintf(txt + n, sizeof txt - n, "\nBMI %.1f (%s)", bmi, health_bmi_class(bmi));
    if (health_bmr_kcal()) n += snprintf(txt + n, sizeof txt - n, "\nBMR %d kcal/day", health_bmr_kcal());
    n += snprintf(txt + n, sizeof txt - n, "\nMax HR %d%s, LTHR %d%s\n", health_max_hr(), health_max_hr_auto() ? " (est.)" : "",
                  health_lthr(), health_lthr_auto() ? " (est.)" : "");
    n += snprintf(txt + n, sizeof txt - n, "\nHR zones, %% of %s:", c->hr_zone_mode == HRZ_PCT_LTHR ? "LTHR" : "max HR");
    for (int z = 1; z <= HR_ZONES; z++) {
        if (z < HR_ZONES) snprintf(line, sizeof line, "\n Z%d  %d-%d  %s", z, health_hr_zone_low(z), health_hr_zone_low(z + 1) - 1, k_hr_names[z - 1]);
        else snprintf(line, sizeof line, "\n Z%d  %d+  %s", z, health_hr_zone_low(z), k_hr_names[z - 1]);
        n += snprintf(txt + n, sizeof txt - n, "%s", line);
    }
    if (health_pwr_zones_available()) {
        n += snprintf(txt + n, sizeof txt - n, "\n\nPower zones, FTP %u W:", c->ftp_w);
        for (int z = 1; z <= PWR_ZONES; z++) {
            if (z < PWR_ZONES) snprintf(line, sizeof line, "\n Z%d  %d-%d  %s", z, health_pwr_zone_low(z), health_pwr_zone_low(z + 1) - 1, k_pwr_names[z - 1]);
            else snprintf(line, sizeof line, "\n Z%d  %d+  %s", z, health_pwr_zone_low(z), k_pwr_names[z - 1]);
            n += snprintf(txt + n, sizeof txt - n, "%s", line);
        }
    } else {
        n += snprintf(txt + n, sizeof txt - n, "\n\nPower zones: set FTP");
    }
    lv_obj_t *l = label(box, &lv_font_montserrat_14, C_FG, txt);
    lv_obj_add_style(l, &theme_st_text, 0);
    lv_obj_set_width(l, LCD_H_RES - 20);
    lv_obj_set_pos(l, 10, 8);
    lv_obj_t *pad = lv_obj_create(box);   /* room to scroll the last line clear of the edge */
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 1, 12);
    lv_obj_align_to(pad, l, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);
    update_hl(s);
}

enum { PROF_WEIGHT, PROF_HEIGHT, PROF_BIRTH, PROF_SEX, PROF_MAXHR, PROF_LTHR, PROF_FTP, PROF_ZONEMODE, PROF_BIKE, PROF_HEALTH };

static const char *sex_text(void) { return config_get()->sex ? "Female" : "Male"; }
static const char *zone_mode_text(void) { return config_get()->hr_zone_mode == HRZ_PCT_LTHR ? "%LTHR" : "%Max HR"; }

static void profile_refresh(screen_t *s)
{
    char buf[24];
    weight_text(buf, sizeof buf); set_right(s, PROF_WEIGHT, buf, true);
    height_text(buf, sizeof buf); set_right(s, PROF_HEIGHT, buf, true);
    birth_text(buf, sizeof buf);  set_right(s, PROF_BIRTH, buf, true);
    set_right(s, PROF_SEX, sex_text(), false);
    maxhr_text(buf, sizeof buf);  set_right(s, PROF_MAXHR, buf, true);
    lthr_text(buf, sizeof buf);   set_right(s, PROF_LTHR, buf, true);
    ftp_text(buf, sizeof buf);    set_right(s, PROF_FTP, buf, true);
    set_right(s, PROF_ZONEMODE, zone_mode_text(), false);
    bike_text(buf, sizeof buf);   set_right(s, PROF_BIKE, buf, true);
}

static void profile_select(screen_t *s, int idx)
{
    switch (idx) {
    case PROF_WEIGHT: value_open("Weight", &k_weight_value); break;
    case PROF_HEIGHT: value_open("Height", &k_height_value); break;
    case PROF_BIRTH:  value_open("Year of birth", &k_birth_value); break;
    case PROF_SEX:
        config_get()->sex = !config_get()->sex;
        config_save();
        profile_refresh(s);
        break;
    case PROF_MAXHR:  value_open("Max heart rate", &k_maxhr_value); break;
    case PROF_LTHR:   value_open("Threshold HR", &k_lthr_value); break;
    case PROF_FTP:    value_open("FTP", &k_ftp_value); break;
    case PROF_ZONEMODE:
        config_get()->hr_zone_mode = config_get()->hr_zone_mode == HRZ_PCT_LTHR ? HRZ_PCT_MAX : HRZ_PCT_LTHR;
        config_save();
        profile_refresh(s);
        break;
    case PROF_BIKE:   value_open("Bike weight", &k_bike_value); break;
    case PROF_HEALTH: health_open(); break;
    default: break;
    }
}

static void profile_open(void)
{
    screen_t *s = push("Profile");
    if (!s) return;
    s->select_cb = profile_select;
    s->refresh_cb = profile_refresh;
    make_list(s);
    add_item(s, "Weight", ITEM_ARROW, "", false);
    add_item(s, "Height", ITEM_ARROW, "", false);
    add_item(s, "Year of birth", ITEM_ARROW, "", false);
    add_item(s, "Sex", ITEM_VALUE, "", false);
    add_item(s, "Max HR", ITEM_ARROW, "", false);
    add_item(s, "LTHR", ITEM_ARROW, "", false);
    add_item(s, "FTP", ITEM_ARROW, "", false);
    add_item(s, "HR zones by", ITEM_VALUE, "", false);
    add_item(s, "Bike weight", ITEM_ARROW, "", false);
    add_item(s, "Health", ITEM_ARROW, NULL, false);
    s->sel = 0;
    profile_refresh(s);
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
    lv_label_set_text_fmt(l, BOARD_NAME " open firmware\n%s\n\nESP-IDF %s\nbuilt %s\n\nheap %u KB  psram %u KB\nup %lu s",
                          d->version, d->idf_ver, d->date,
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          (unsigned long)(esp_timer_get_time() / 1000000));
    lv_obj_set_width(l, LCD_H_RES - 20);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 10, HDR_H + 10);
    update_hl(s);
}

/* "Reset settings?" confirmation: a two-item list so keys work too. */
static void reset_confirm_select(screen_t *s, int idx)
{
    if (idx != 1) {
        pop();
        return;
    }
    config_defaults(config_get());
    config_save();
    theme_set(config_get()->theme);
    if (s_action_cb[MENU_ACTION_BACKLIGHT]) s_action_cb[MENU_ACTION_BACKLIGHT]();
    menu_close();   /* the close callback rebuilds the pages */
}

static void reset_confirm_open(void)
{
    screen_t *s = push("Reset settings?");
    if (!s) return;
    s->select_cb = reset_confirm_select;
    make_list(s);
    lv_obj_t *h = label(s->root, &lv_font_montserrat_14, C_GREY,
                        "Pages, profile, lap length,\ntime zone, theme and backlight\ngo back to the firmware defaults.");
    lv_obj_add_style(h, &theme_st_muted, 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, HDR_H + 2 * ROW_H + 16);
    add_item(s, "Cancel", ITEM_PLAIN, NULL, false);
    add_item(s, LV_SYMBOL_WARNING "  Reset to defaults", ITEM_PLAIN, NULL, false);
    s->sel = 0;
    update_hl(s);
}

static void system_select(screen_t *s, int idx)
{
    void (*cb)(void) = NULL;
    switch (idx) {
    case 0: cb = s_action_cb[MENU_ACTION_USB]; break;
    case 1: cb = s_action_cb[MENU_ACTION_POWER_OFF]; break;
    case 2: reset_confirm_open(); return;
    case 3: about_open(); return;
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
    add_item(s, "Reset settings", ITEM_ARROW, NULL, false);
    add_item(s, "About", ITEM_ARROW, NULL, false);
    s->sel = 0;
    update_hl(s);
}

/* ---- settings root ----------------------------------------------------- */
enum { ROOT_PAGES, ROOT_SENSORS, ROOT_PROFILE, ROOT_LAP, ROOT_AUTOPAUSE, ROOT_BACKLIGHT, ROOT_TZ, ROOT_THEME, ROOT_AUTOTHEME,
       ROOT_LAYERS, ROOT_ROUTE, ROOT_HISTORY, ROOT_RESET, ROOT_SYSTEM };

/* ---- route: GPX files in /sdcard/c606oss/routes ------------------------ */

static char s_routes[MAX_ITEMS - 2][ROUTE_NAME_MAX];
static int s_nroutes;
static bool s_route_fit;   /* the loaded route is a recorded ride: extra row on top */

/* Preview of one file before it becomes the route: the track outline
 * (thinned to PREVIEW_POINTS), start / end markers, length, climb and
 * descent from <ele> when present, a Reverse toggle (swaps the markers and
 * the climb / descent) and "Use this route". */
#define PREVIEW_POINTS 512
#define PREVIEW_X 12
#define PREVIEW_Y (HDR_H + 4)
#define PREVIEW_W (LCD_H_RES - 2 * PREVIEW_X)
#define PREVIEW_H 160
#define PREVIEW_PAD 6

static route_info_t s_prev;
static char s_prev_name[ROUTE_NAME_MAX];
static bool s_prev_rev;
static lv_obj_t *s_prev_start, *s_prev_end, *s_prev_stats;
static lv_point_precise_t *s_prev_pts;     /* thumbnail points (PSRAM) */

static void preview_close(screen_t *s)
{
    route_info_free(&s_prev);
    free(s_prev_pts);
    s_prev_pts = NULL;
}

static void preview_update(void)
{
    char buf[96];
    int n = snprintf(buf, sizeof buf, "%.1f km   %lu points\n", s_prev.len_m / 1000, (unsigned long)s_prev.npoints);
    if (s_prev.has_ele) {
        float up = s_prev_rev ? s_prev.descent_m : s_prev.climb_m;
        float down = s_prev_rev ? s_prev.climb_m : s_prev.descent_m;
        snprintf(buf + n, sizeof buf - n, LV_SYMBOL_UP " %.0f m   " LV_SYMBOL_DOWN " %.0f m   (%.0f-%.0f m)",
                 up, down, s_prev.min_ele_m, s_prev.max_ele_m);
    } else {
        snprintf(buf + n, sizeof buf - n, "No elevation data");
    }
    lv_label_set_text(s_prev_stats, buf);
    if (s_prev_pts && s_prev.n && s_prev_start) {
        const lv_point_precise_t *a = &s_prev_pts[0], *b = &s_prev_pts[s_prev.n - 1];
        if (s_prev_rev) { const lv_point_precise_t *t = a; a = b; b = t; }
        lv_obj_set_pos(s_prev_start, (int32_t)a->x - 4, (int32_t)a->y - 4);
        lv_obj_set_pos(s_prev_end, (int32_t)b->x - 4, (int32_t)b->y - 4);
    }
}

static void preview_select(screen_t *s, int idx)
{
    if (idx == 0) {
        s_prev_rev = !s_prev_rev;
        set_toggle(s, 0, s_prev_rev);
        preview_update();
        return;
    }
    app_cfg_t *c = config_get();
    strncpy(c->route, s_prev_name, sizeof c->route - 1);
    c->route_reverse = s_prev_rev;
    config_save();
    if (route_load(c->route, c->route_reverse) != ESP_OK) { c->route[0] = 0; config_save(); }
    pop();      /* the preview */
    pop();      /* the file list: back to the settings root */
}

static lv_obj_t *marker(lv_obj_t *parent, lv_palette_t p)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, 9, 9);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m, lv_palette_main(p), 0);
    lv_obj_set_style_border_width(m, 1, 0);
    lv_obj_set_style_border_color(m, lv_color_white(), 0);
    return m;
}

/* The track outline of s_prev in a panel: the thinned points scaled to
 * fit, start (green) / end (red) markers. */
static lv_obj_t *outline_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_add_style(box, &theme_st_panel, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 4, 0);

    s_prev_pts = s_prev.n ? heap_caps_malloc(s_prev.n * sizeof *s_prev_pts, MALLOC_CAP_SPIRAM) : NULL;
    if (s_prev_pts) {
        int32_t minx = s_prev.x20[0], maxx = minx, miny = s_prev.y20[0], maxy = miny;
        for (size_t i = 1; i < s_prev.n; i++) {
            if (s_prev.x20[i] < minx) minx = s_prev.x20[i];
            if (s_prev.x20[i] > maxx) maxx = s_prev.x20[i];
            if (s_prev.y20[i] < miny) miny = s_prev.y20[i];
            if (s_prev.y20[i] > maxy) maxy = s_prev.y20[i];
        }
        /* fit the bounding box, same scale on both axes, centred */
        float bw = (float)(maxx - minx), bh = (float)(maxy - miny);
        float aw = w - 2 * PREVIEW_PAD, ah = h - 2 * PREVIEW_PAD;
        float sc = 1.0f;
        if (bw > 0 || bh > 0) sc = fminf(bw > 0 ? aw / bw : 1e9f, bh > 0 ? ah / bh : 1e9f);
        float ox = PREVIEW_PAD + (aw - bw * sc) / 2, oy = PREVIEW_PAD + (ah - bh * sc) / 2;
        for (size_t i = 0; i < s_prev.n; i++) {
            s_prev_pts[i].x = ox + (s_prev.x20[i] - minx) * sc;
            s_prev_pts[i].y = oy + (s_prev.y20[i] - miny) * sc;
        }
        lv_obj_t *line = lv_line_create(box);
        lv_line_set_points(line, s_prev_pts, s_prev.n);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_color(line, theme_current() == THEME_DARK ? lv_color_make(0xe0, 0x50, 0xd0)
                                                                          : lv_color_make(0xc0, 0x20, 0xa0), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        s_prev_start = marker(box, LV_PALETTE_GREEN);
        s_prev_end = marker(box, LV_PALETTE_RED);
    } else {
        s_prev_start = s_prev_end = NULL;
        lv_obj_t *l = label(box, &lv_font_montserrat_14, C_GREY, "No GPS track");
        lv_obj_add_style(l, &theme_st_muted, 0);
        lv_obj_center(l);
    }
    return box;
}

static void route_preview_open(const char *name)
{
    route_info_t info;
    if (route_scan(name, PREVIEW_POINTS, &info) != ESP_OK) return;

    char title[28];
    snprintf(title, sizeof title, "%.24s%s", name, strlen(name) > 24 ? "..." : "");
    screen_t *s = push(title);
    if (!s) { route_info_free(&info); return; }
    s_prev = info;
    strncpy(s_prev_name, name, sizeof s_prev_name - 1);
    s_prev_rev = config_get()->route_reverse && !strcmp(name, config_get()->route);
    s->select_cb = preview_select;
    s->close_cb = preview_close;

    outline_create(s->root, PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H);

    s_prev_stats = label(s->root, &lv_font_montserrat_14, C_FG, "");
    lv_obj_add_style(s_prev_stats, &theme_st_text, 0);
    lv_obj_set_style_text_align(s_prev_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_prev_stats, LCD_H_RES);
    lv_obj_set_pos(s_prev_stats, 0, PREVIEW_Y + PREVIEW_H + 4);

    make_list(s);
    lv_obj_set_pos(s->list, 0, LCD_V_RES - 2 * ROW_H);
    lv_obj_set_size(s->list, LCD_H_RES, 2 * ROW_H);
    add_item(s, "Reverse", ITEM_TOGGLE, NULL, s_prev_rev);
    add_item(s, LV_SYMBOL_OK "  Use this route", ITEM_PLAIN, NULL, false);
    s->sel = 1;
    update_hl(s);
    preview_update();
}

static void route_select(screen_t *s, int idx)
{
    app_cfg_t *c = config_get();
    if (idx == 0) {
        c->route[0] = 0;
        config_save();
        route_clear();
        pop();
    } else if (s_route_fit && idx == 1) {
        route_preview_open(c->route);
    } else if (idx - s_route_fit <= s_nroutes) {
        route_preview_open(s_routes[idx - 1 - s_route_fit]);
    }
}

static void route_open(void)
{
    screen_t *s = push("Route");
    if (!s) return;
    s->select_cb = route_select;
    make_list(s);
    s_nroutes = route_list(s_routes, MAX_ITEMS - 2);
    /* first row: clears the loaded route (reads "None" while nothing is loaded) */
    add_item(s, route_loaded() ? LV_SYMBOL_CLOSE "  Unload route" : "None", ITEM_PLAIN, NULL, false);
    s->sel = 0;
    /* a recorded ride used as the route (from History) is not in the list: show it on top */
    s_route_fit = route_loaded() && route_is_fit(config_get()->route);
    if (s_route_fit) {
        add_item(s, config_get()->route, ITEM_VALUE, config_get()->route_reverse ? LV_SYMBOL_OK " rev." : LV_SYMBOL_OK, false);
        s->sel = 1;
    }
    for (int i = 0; i < s_nroutes; i++) {
        bool cur = route_loaded() && !strcmp(s_routes[i], config_get()->route);
        add_item(s, s_routes[i], cur ? ITEM_VALUE : ITEM_PLAIN,
                 config_get()->route_reverse ? LV_SYMBOL_OK " rev." : LV_SYMBOL_OK, false);
        if (cur) s->sel = i + 1 + s_route_fit;
    }
    if (!s_nroutes) {
        lv_obj_t *h = label(s->root, &lv_font_montserrat_14, C_GREY,
                            "Copy .gpx files to\nc606oss/routes on the\nUSB disk");
        lv_obj_add_style(h, &theme_st_muted, 0);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, HDR_H + ROW_H + 16);
    }
    update_hl(s);
}

static const char *route_text(void)
{
    static char buf[24];
    if (!route_loaded()) return "none";
    snprintf(buf, sizeof buf, "%.1f km", route_length_m() / 1000);
    return buf;
}

/* ---- history: recorded rides ------------------------------------------- */

static history_entry_t s_hist[MAX_ITEMS];
static int s_nhist;

/* ride screen: outline on top, a 3 x 4 grid of summary values, then
 * "Use as route" / "Delete ride" */
#define RIDE_OUTLINE_H 72
#define RIDE_GRID_Y    (PREVIEW_Y + RIDE_OUTLINE_H + 4)
#define RIDE_COLS      3
#define RIDE_ROWS      4
#define RIDE_CELL_W    (LCD_H_RES / RIDE_COLS)
#define RIDE_CELL_H    ((LCD_V_RES - 2 * ROW_H - RIDE_GRID_Y) / RIDE_ROWS)

static void hms(char *buf, size_t n, uint32_t ms)
{
    uint32_t sec = ms / 1000;
    snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
}

static void ride_cell(lv_obj_t *parent, int idx, const char *name, const char *value)
{
    int x = (idx % RIDE_COLS) * RIDE_CELL_W, y = RIDE_GRID_Y + (idx / RIDE_COLS) * RIDE_CELL_H;
    lv_obj_t *n = label(parent, &lv_font_montserrat_14, C_GREY, name);
    lv_obj_add_style(n, &theme_st_muted, 0);
    lv_obj_set_pos(n, x + 8, y);
    lv_obj_t *v = label(parent, &lv_font_montserrat_14, C_FG, value);
    lv_obj_add_style(v, &theme_st_text, 0);
    lv_obj_set_pos(v, x + 8, y + 15);
}

static void ride_use_as_route(void)
{
    app_cfg_t *c = config_get();
    strncpy(c->route, s_prev_name, sizeof c->route - 1);
    c->route_reverse = 0;
    config_save();
    if (route_load(c->route, 0) != ESP_OK) { c->route[0] = 0; config_save(); }
    pop();      /* the ride */
    pop();      /* the history list: back to the settings root */
}

static void delete_confirm_select(screen_t *s, int idx)
{
    if (idx != 1) {
        pop();
        return;
    }
    app_cfg_t *c = config_get();
    if (history_delete(s_prev_name) == ESP_OK && !strcmp(c->route, s_prev_name)) {
        c->route[0] = 0;   /* it was the map's route */
        config_save();
        route_clear();
    }
    pop();      /* the confirmation */
    pop();      /* the ride: the list refreshes itself */
}

static void delete_confirm_open(void)
{
    screen_t *s = push("Delete ride?");
    if (!s) return;
    s->select_cb = delete_confirm_select;
    make_list(s);
    char buf[96];
    const char *base = strrchr(s_prev_name, '/');
    snprintf(buf, sizeof buf, "%.40s\nwill be removed from\nthe card for good.", base ? base + 1 : s_prev_name);
    lv_obj_t *h = label(s->root, &lv_font_montserrat_14, C_GREY, buf);
    lv_obj_add_style(h, &theme_st_muted, 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, HDR_H + 2 * ROW_H + 16);
    add_item(s, "Cancel", ITEM_PLAIN, NULL, false);
    add_item(s, LV_SYMBOL_TRASH "  Delete", ITEM_PLAIN, NULL, false);
    s->sel = 0;
    update_hl(s);
}

static void ride_select(screen_t *s, int idx)
{
    bool has_track = s_prev.n >= 2;
    if (has_track && idx == 0) ride_use_as_route();
    else if (idx == has_track) delete_confirm_open();
}

static void ride_open(const history_entry_t *e)
{
    route_info_t info;
    if (route_scan(e->name, PREVIEW_POINTS, &info) != ESP_OK) return;

    char title[40];
    history_label(e, title, sizeof title);
    screen_t *s = push(title);
    if (!s) { route_info_free(&info); return; }
    s_prev = info;
    strncpy(s_prev_name, e->name, sizeof s_prev_name - 1);
    s->select_cb = ride_select;
    s->close_cb = preview_close;

    outline_create(s->root, PREVIEW_X, PREVIEW_Y, PREVIEW_W, RIDE_OUTLINE_H);
    if (s_prev_start) {
        lv_obj_set_pos(s_prev_start, (int32_t)s_prev_pts[0].x - 4, (int32_t)s_prev_pts[0].y - 4);
        lv_obj_set_pos(s_prev_end, (int32_t)s_prev_pts[s_prev.n - 1].x - 4, (int32_t)s_prev_pts[s_prev.n - 1].y - 4);
    }

    const fit_summary_t *f = &s_prev.fit;
    char v[16];
    hms(v, sizeof v, f->timer_ms);
    ride_cell(s->root, 0, "Time", v);
    /* the track length when the file carries no distance (no wheel sensor, no GPS distance) */
    float dist = f->distance_m > 0 ? f->distance_m : s_prev.len_m;
    snprintf(v, sizeof v, "%.1f km", dist / 1000);
    ride_cell(s->root, 1, "Distance", v);
    if (f->avg_speed_ms >= 0) snprintf(v, sizeof v, "%.1f km/h", f->avg_speed_ms * 3.6f); else strcpy(v, "--");
    ride_cell(s->root, 2, "Avg spd", v);
    if (f->max_speed_ms >= 0) snprintf(v, sizeof v, "%.1f km/h", f->max_speed_ms * 3.6f); else strcpy(v, "--");
    ride_cell(s->root, 3, "Max spd", v);
    if (f->avg_hr >= 0) snprintf(v, sizeof v, "%d bpm", f->avg_hr); else strcpy(v, "--");
    ride_cell(s->root, 4, "Avg HR", v);
    if (f->max_hr >= 0) snprintf(v, sizeof v, "%d bpm", f->max_hr); else strcpy(v, "--");
    ride_cell(s->root, 5, "Max HR", v);
    if (f->avg_cad >= 0) snprintf(v, sizeof v, "%d rpm", f->avg_cad); else strcpy(v, "--");
    ride_cell(s->root, 6, "Cadence", v);
    if (f->avg_power >= 0) snprintf(v, sizeof v, "%d W", f->avg_power); else strcpy(v, "--");
    ride_cell(s->root, 7, "Power", v);
    if (f->calories >= 0) snprintf(v, sizeof v, "%d kcal", f->calories); else strcpy(v, "--");
    ride_cell(s->root, 8, "Calories", v);
    int up = f->ascent_m >= 0 ? f->ascent_m : s_prev.has_ele ? (int)(s_prev.climb_m + 0.5f) : -1;
    int down = f->descent_m >= 0 ? f->descent_m : s_prev.has_ele ? (int)(s_prev.descent_m + 0.5f) : -1;
    if (up >= 0) snprintf(v, sizeof v, "%d m", up); else strcpy(v, "--");
    ride_cell(s->root, 9, "Climb", v);
    if (down >= 0) snprintf(v, sizeof v, "%d m", down); else strcpy(v, "--");
    ride_cell(s->root, 10, "Descent", v);
    snprintf(v, sizeof v, "%d", f->laps);
    ride_cell(s->root, 11, "Laps", v);

    make_list(s);
    lv_obj_set_pos(s->list, 0, LCD_V_RES - 2 * ROW_H);
    lv_obj_set_size(s->list, LCD_H_RES, 2 * ROW_H);
    if (s_prev.n >= 2) add_item(s, LV_SYMBOL_GPS "  Use as route", ITEM_PLAIN, NULL, false);
    add_item(s, LV_SYMBOL_TRASH "  Delete ride", ITEM_PLAIN, NULL, false);
    s->sel = 0;
    update_hl(s);
}

static void history_select(screen_t *s, int idx)
{
    if (idx < s_nhist) ride_open(&s_hist[idx]);
}

static void history_fill(screen_t *s)
{
    int total;
    s_nhist = history_list(s_hist, MAX_ITEMS - 1, &total);
    char buf[40];
    for (int i = 0; i < s_nhist; i++) {
        history_label(&s_hist[i], buf, sizeof buf);
        add_item(s, buf, ITEM_ARROW, s_hist[i].vendor ? "Magene" : NULL, false);
    }
    if (total > s_nhist) {
        snprintf(buf, sizeof buf, "%d older not shown", total - s_nhist);
        add_item(s, buf, ITEM_PLAIN, NULL, false);
    }
    if (!s_nhist) {
        lv_obj_t *h = label(s->list, &lv_font_montserrat_14, C_GREY, "No rides recorded yet");
        lv_obj_add_style(h, &theme_st_muted, 0);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(h, LCD_H_RES);
        lv_obj_set_style_pad_top(h, ROW_H, 0);
    }
}

static void history_refresh(screen_t *s)
{
    int sel = s->sel;
    list_clear(s);
    history_fill(s);
    s->sel = sel < s->n ? sel : s->n - 1;
    update_hl(s);
}

static void history_open(void)
{
    screen_t *s = push("History");
    if (!s) return;
    s->select_cb = history_select;
    s->refresh_cb = history_refresh;
    make_list(s);
    history_fill(s);
    s->sel = s_nhist ? 0 : -1;
    update_hl(s);
}

/* ---- map layers: a toggle per layer group ------------------------------ */

static void layers_select(screen_t *s, int idx)
{
    if (idx >= mapview_layer_count()) return;
    config_get()->map_layers ^= 1u << idx;
    config_save();
    set_toggle(s, idx, config_get()->map_layers & (1u << idx));
}

static void layers_open(void)
{
    screen_t *s = push("Map layers");
    if (!s) return;
    s->select_cb = layers_select;
    make_list(s);
    for (int i = 0; i < mapview_layer_count(); i++) {
        add_item(s, mapview_layer_name(i), ITEM_TOGGLE, NULL, config_get()->map_layers & (1u << i));
    }
    s->sel = 0;
    update_hl(s);
}

static void settings_select(screen_t *s, int idx)
{
    switch (idx) {
    case ROOT_PAGES:
        pages_open();
        break;
    case ROOT_SENSORS:
        sensors_open();
        break;
    case ROOT_PROFILE:
        profile_open();
        break;
    case ROOT_LAP:
        value_open("Lap length", &k_lap_value);
        break;
    case ROOT_AUTOPAUSE:
        config_get()->auto_pause = !config_get()->auto_pause;
        config_save();
        set_toggle(s, idx, config_get()->auto_pause);
        break;
    case ROOT_BACKLIGHT:
        value_open("Backlight", &k_bl_value);
        break;
    case ROOT_TZ:
        value_open("Time zone", &k_tz_value);
        break;
    case ROOT_THEME:
        /* a manual choice ends the automatic mode */
        config_get()->theme = theme_current() == THEME_LIGHT ? THEME_DARK : THEME_LIGHT;
        config_get()->theme_auto = 0;
        config_save();
        theme_set(config_get()->theme);
        set_right(s, idx, theme_name(theme_current()), false);
        set_toggle(s, ROOT_AUTOTHEME, false);
        update_hl(s);   /* rows keep local colours: re-apply for the new theme */
        break;
    case ROOT_AUTOTHEME: {
        /* light between sunrise and sunset, dark otherwise (ui.c keeps it
         * up to date); apply right away when the sun times are known */
        bool day;
        config_get()->theme_auto = !config_get()->theme_auto;
        config_save();
        set_toggle(s, idx, config_get()->theme_auto);
        theme_id_t want = config_get()->theme;
        if (config_get()->theme_auto && sun_is_day(&day)) want = day ? THEME_LIGHT : THEME_DARK;
        if (want != theme_current()) {
            theme_set(want);
            set_right(s, ROOT_THEME, theme_name(want), false);
            update_hl(s);
        }
        break;
    }
    case ROOT_LAYERS:
        layers_open();
        break;
    case ROOT_ROUTE:
        route_open();
        break;
    case ROOT_HISTORY:
        history_open();
        break;
    case ROOT_RESET:
        stats_reset();
        health_reset();
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
    set_right(s, ROOT_ROUTE, route_text(), true);
}

static void timer_cb(lv_timer_t *t)
{
    if (s_dp_layout.cont) datapage_refresh(&s_dp_layout);
    if (s_dp_fields.cont) datapage_refresh(&s_dp_fields);
    screen_t *s = top();
    if (s && s->live && s->refresh_cb) s->refresh_cb(s);
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
    add_item(s, "Sensors", ITEM_ARROW, NULL, false);
    add_item(s, "Profile", ITEM_ARROW, NULL, false);
    lap_text(buf, sizeof buf);
    add_item(s, "Lap length", ITEM_ARROW, buf, false);
    add_item(s, "Auto pause", ITEM_TOGGLE, NULL, config_get()->auto_pause);
    bl_text(buf, sizeof buf);
    add_item(s, "Backlight", ITEM_ARROW, buf, false);
    tz_text(buf, sizeof buf);
    add_item(s, "Time zone", ITEM_ARROW, buf, false);
    add_item(s, "Theme", ITEM_VALUE, theme_name(theme_current()), false);
    add_item(s, "Auto theme", ITEM_TOGGLE, NULL, config_get()->theme_auto);
    add_item(s, "Map layers", ITEM_ARROW, NULL, false);
    add_item(s, "Route", ITEM_ARROW, route_text(), false);
    add_item(s, "History", ITEM_ARROW, NULL, false);
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
        screen_t *s = &s_stack[--s_depth];
        if (s->close_cb) s->close_cb(s);
        lv_obj_delete(s->root);
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
    if (evt == KEY_EVT_LONG_START && s->key_cb == value_key && (key == KEY_MENU_UP || key == KEY_MENU_DOWN)) {
        value_key(s, key);                   /* hold up / down to run the value */
        return true;
    }
    if (evt != KEY_EVT_CLICK) return true;   /* other holds are handled by main.c before we see them */
    if (s->key_cb) {
        s->key_cb(s, key);
        return true;
    }
    if (key == KEY_MENU_UP && s->sel > -1) s->sel--;
    else if (key == KEY_MENU_DOWN && s->sel < s->n - 1) s->sel++;
    else if (key == KEY_MENU_BACK) {
        pop();
        return true;
    } else if (key == KEY_MENU_SELECT) {
        if (s->sel < 0) pop();
        else if (s->select_cb) s->select_cb(s, s->sel);
        return true;
    }
    update_hl(s);
    return true;
}
