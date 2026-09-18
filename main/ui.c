/* UI: a status page (battery arc, environment, key indicators, event log,
 * heap stats), a "ride" page with big digits and the user-configurable data
 * pages (config.c: layout + field per cell, edited in the settings menu).
 * Key 0 cycles through the pages; the gear icon opens the menu. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "board.h"
#include "ui_port.h"
#include "ui.h"
#include "stats.h"
#include "config.h"
#include "datapage.h"
#include "menu.h"

#define LOG_LINES 2
#define MAX_PAGES (2 + CFG_PAGES)
#define HDR_H 28

static lv_obj_t *s_page_status, *s_page_ride;
static lv_obj_t *s_pages[MAX_PAGES];
static int s_npages, s_page_idx;
static datapage_t s_dp[CFG_PAGES];          /* one per configured page */
static int s_dp_of_page[MAX_PAGES];         /* page index -> config page */
static lv_obj_t *s_data_bat[MAX_PAGES];
static uint8_t s_bat_pct;
static lv_obj_t *s_hdr_bat, *s_hdr_rec, *s_arc, *s_arc_lbl, *s_env, *s_nrf, *s_gps, *s_sd, *s_log, *s_foot;
static lv_obj_t *s_key[3];
static lv_obj_t *s_big, *s_big_caption, *s_ride_env, *s_ride_gps, *s_ride_pos, *s_ride_time, *s_ride_rec;
static lv_obj_t *s_cursor, *s_touch_lbl, *s_btn_rec, *s_btn_usb, *s_ride_sens, *s_ant;
static ui_action_cb_t s_on_rec, s_on_usb, s_on_power_off;
static lv_obj_t *s_popup;
static lv_timer_t *s_popup_timer;

static char s_log_buf[LOG_LINES][32];
static int s_log_n;
static uint8_t s_bl_pct = 70;
static uint32_t s_uptime;

static const lv_color_t C_BG   = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t C_HDR  = LV_COLOR_MAKE(0x10, 0x40, 0xa0);
static const lv_color_t C_IDLE = LV_COLOR_MAKE(0x30, 0x30, 0x30);
static const lv_color_t C_HOLD = LV_COLOR_MAKE(0xd0, 0x20, 0x20);
static const lv_color_t C_CLK  = LV_COLOR_MAKE(0x20, 0xa0, 0x30);

static void gear_button(lv_obj_t *hdr);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(p, C_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    return p;
}

static void build_status(lv_obj_t *scr)
{
    s_page_status = page(scr);

    /* header */
    lv_obj_t *hdr = lv_obj_create(s_page_status);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LCD_H_RES, 28);
    lv_obj_set_style_bg_color(hdr, C_HDR, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_t *t = label(hdr, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(t, "C606 open FW");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 6, 0);
    s_hdr_bat = label(hdr, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(s_hdr_bat, "--%");
    lv_obj_align(s_hdr_bat, LV_ALIGN_RIGHT_MID, -6, 0);
    s_hdr_rec = label(hdr, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_RED));
    lv_label_set_text(s_hdr_rec, "");
    lv_obj_align(s_hdr_rec, LV_ALIGN_CENTER, 0, 0);
    gear_button(hdr);

    /* battery arc */
    s_arc = lv_arc_create(s_page_status);
    lv_obj_set_size(s_arc, 120, 120);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 34);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_arc, false);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, C_IDLE, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, C_CLK, LV_PART_INDICATOR);
    s_arc_lbl = label(s_arc, &lv_font_montserrat_20, lv_color_white());
    lv_label_set_text(s_arc_lbl, "--\n-- mV");
    lv_obj_set_style_text_align(s_arc_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_arc_lbl);

    s_env = label(s_page_status, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(s_env, "no sensor data yet");
    lv_obj_align(s_env, LV_ALIGN_TOP_MID, 0, 160);

    s_nrf = label(s_page_status, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_ORANGE));
    lv_label_set_text(s_nrf, "nRF: no reply yet");
    lv_obj_align(s_nrf, LV_ALIGN_TOP_MID, 0, 178);

    s_gps = label(s_page_status, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_ORANGE));
    lv_label_set_text(s_gps, "GPS: probing baud");
    lv_obj_align(s_gps, LV_ALIGN_TOP_MID, 0, 196);

    s_sd = label(s_page_status, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_ORANGE));
    lv_label_set_text(s_sd, "SD: not mounted");
    lv_obj_align(s_sd, LV_ALIGN_TOP_MID, 0, 214);

    /* key indicators */
    for (int i = 0; i < 3; i++) {
        s_key[i] = lv_obj_create(s_page_status);
        lv_obj_remove_style_all(s_key[i]);
        lv_obj_set_size(s_key[i], 64, 26);
        lv_obj_set_style_bg_color(s_key[i], C_IDLE, 0);
        lv_obj_set_style_bg_opa(s_key[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_key[i], 6, 0);
        lv_obj_align(s_key[i], LV_ALIGN_TOP_LEFT, 12 + i * 76, 236);
        lv_obj_t *l = label(s_key[i], &lv_font_montserrat_14, lv_color_white());
        lv_label_set_text_fmt(l, "key %d", i);
        lv_obj_center(l);
    }

    /* event log */
    s_log = label(s_page_status, &lv_font_unscii_8, lv_palette_lighten(LV_PALETTE_GREY, 2));
    lv_obj_set_width(s_log, LCD_H_RES - 16);
    lv_obj_align(s_log, LV_ALIGN_TOP_LEFT, 8, 268);
    lv_label_set_text(s_log, "press a key...");

    s_touch_lbl = label(s_page_status, &lv_font_unscii_8, lv_palette_main(LV_PALETTE_CYAN));
    lv_obj_align(s_touch_lbl, LV_ALIGN_TOP_LEFT, 8, 290);
    lv_label_set_text(s_touch_lbl, "touch: none");

    s_ant = label(s_page_status, &lv_font_unscii_8, lv_palette_main(LV_PALETTE_PINK));
    lv_obj_align(s_ant, LV_ALIGN_TOP_LEFT, 120, 290);
    lv_label_set_text(s_ant, "ANT: waiting for nRF");

    /* footer */
    s_foot = label(s_page_status, &lv_font_unscii_8, lv_palette_main(LV_PALETTE_GREY));
    lv_obj_align(s_foot, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_text(s_foot, "");
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_color_t color, void *unused)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 96, 36);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_t *l = label(b, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void build_ride(lv_obj_t *scr)
{
    s_page_ride = page(scr);
    lv_obj_set_hidden(s_page_ride, true);

    s_ride_time = label(s_page_ride, &lv_font_montserrat_20, lv_palette_main(LV_PALETTE_GREY));
    lv_label_set_text(s_ride_time, "--:--:-- UTC");
    lv_obj_align(s_ride_time, LV_ALIGN_TOP_MID, 0, 12);

    s_big = label(s_page_ride, &lv_font_montserrat_48, lv_color_white());
    lv_label_set_text(s_big, "0.0");
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, 56);

    s_big_caption = label(s_page_ride, &lv_font_montserrat_20, lv_palette_main(LV_PALETTE_GREY));
    lv_label_set_text(s_big_caption, "km/h");
    lv_obj_align(s_big_caption, LV_ALIGN_TOP_MID, 0, 112);

    s_ride_gps = label(s_page_ride, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_ORANGE));
    lv_label_set_text(s_ride_gps, "no GPS data");
    lv_obj_align(s_ride_gps, LV_ALIGN_TOP_MID, 0, 150);

    s_ride_pos = label(s_page_ride, &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_style_text_align(s_ride_pos, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ride_pos, "");
    lv_obj_align(s_ride_pos, LV_ALIGN_TOP_MID, 0, 172);

    s_ride_sens = label(s_page_ride, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_PINK));
    lv_label_set_text(s_ride_sens, "HR --  cad --  --.- km/h");
    lv_obj_align(s_ride_sens, LV_ALIGN_TOP_MID, 0, 206);

    s_ride_env = label(s_page_ride, &lv_font_montserrat_20, lv_palette_main(LV_PALETTE_CYAN));
    lv_label_set_text(s_ride_env, "--.- C");
    lv_obj_align(s_ride_env, LV_ALIGN_TOP_MID, 0, 234);

    s_ride_rec = label(s_page_ride, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_RED));
    lv_label_set_text(s_ride_rec, "");
    lv_obj_align(s_ride_rec, LV_ALIGN_TOP_MID, 0, 262);

    /* touch buttons (the user-data pointer is resolved at click time) */
    s_btn_rec = button(s_page_ride, LV_SYMBOL_STOP " REC", lv_palette_main(LV_PALETTE_RED), NULL);
    lv_obj_align(s_btn_rec, LV_ALIGN_BOTTOM_LEFT, 16, -8);
    s_btn_usb = button(s_page_ride, LV_SYMBOL_USB " USB", lv_palette_main(LV_PALETTE_BLUE), NULL);
    lv_obj_align(s_btn_usb, LV_ALIGN_BOTTOM_RIGHT, -16, -8);
}

/* ---- data pages -------------------------------------------------------- */

static void gear_cb(lv_event_t *e) { menu_open(); }

static void gear_button(lv_obj_t *hdr)
{
    lv_obj_t *b = lv_obj_create(hdr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 40, HDR_H);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_obj_add_event_cb(b, gear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(b, &lv_font_montserrat_20, lv_color_white());
    lv_label_set_text(l, LV_SYMBOL_SETTINGS);
    lv_obj_center(l);
}

/* (Re)creates the enabled data pages from the configuration. */
static void build_data_pages(lv_obj_t *scr)
{
    for (int p = 0; p < CFG_PAGES; p++) {
        if (s_dp[p].cont) datapage_delete(&s_dp[p]);
    }
    for (int i = 2; i < s_npages; i++) {
        lv_obj_delete(s_pages[i]);
        s_pages[i] = NULL;
        s_data_bat[i] = NULL;
    }
    s_npages = 2;

    const app_cfg_t *cfg = config_get();
    for (int p = 0; p < CFG_PAGES; p++) {
        if (!cfg->page[p].enabled) continue;
        int idx = s_npages++;
        lv_obj_t *pg = page(scr);
        lv_obj_set_hidden(pg, true);
        s_pages[idx] = pg;
        s_dp_of_page[idx] = p;

        lv_obj_t *hdr = lv_obj_create(pg);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_size(hdr, LCD_H_RES, HDR_H);
        lv_obj_set_style_bg_color(hdr, C_HDR, 0);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
        lv_obj_t *t = label(hdr, &lv_font_montserrat_14, lv_color_white());
        lv_label_set_text_fmt(t, "Page %d", p + 1);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 6, 0);
        s_data_bat[idx] = label(hdr, &lv_font_montserrat_14, lv_color_white());
        lv_label_set_text_fmt(s_data_bat[idx], "%u%%", s_bat_pct);
        lv_obj_align(s_data_bat[idx], LV_ALIGN_RIGHT_MID, -6, 0);
        gear_button(hdr);

        datapage_build(&s_dp[p], pg, &cfg->page[p], 0, HDR_H, LCD_H_RES, LCD_V_RES - HDR_H);
    }
    /* keep the cursor ring above the new pages */
    if (s_cursor) lv_obj_move_foreground(s_cursor);
}

/* Refreshes the visible data page (LVGL task). */
static void data_refresh_cb(lv_timer_t *t)
{
    if (s_page_idx < 2 || s_page_idx >= s_npages || menu_active()) return;
    datapage_refresh(&s_dp[s_dp_of_page[s_page_idx]]);
}

static void show_page(int idx)
{
    for (int i = 0; i < s_npages; i++) {
        lv_obj_set_hidden(s_pages[i], i != idx);
    }
    s_page_idx = idx;
    if (idx >= 2) {
        data_refresh_cb(NULL);   /* don't wait for the timer */
    }
}

/* The configuration may have changed while the menu was open. */
static void on_menu_closed(void)
{
    build_data_pages(lv_screen_active());
    if (s_page_idx >= s_npages) s_page_idx = 0;
    show_page(s_page_idx);
}

static void rec_btn_cb(lv_event_t *e) { if (s_on_rec) s_on_rec(); }
static void usb_btn_cb(lv_event_t *e) { if (s_on_usb) s_on_usb(); }

void ui_set_actions(ui_action_cb_t on_rec, ui_action_cb_t on_usb)
{
    s_on_rec = on_rec;
    s_on_usb = on_usb;
    ui_lock();
    lv_obj_add_event_cb(s_btn_rec, rec_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn_usb, usb_btn_cb, LV_EVENT_CLICKED, NULL);
    ui_unlock();
}

static void popup_timeout_cb(lv_timer_t *t)
{
    s_popup_timer = NULL;
    ui_hide_power_popup();
}

static void popup_off_cb(lv_event_t *e) { if (s_on_power_off) s_on_power_off(); }
static void popup_cancel_cb(lv_event_t *e) { ui_hide_power_popup(); }

void ui_set_power_off_cb(ui_action_cb_t cb) { s_on_power_off = cb; }

bool ui_power_popup_active(void) { return s_popup != NULL; }

void ui_hide_power_popup(void)
{
    ui_lock();
    if (s_popup_timer) { lv_timer_delete(s_popup_timer); s_popup_timer = NULL; }
    if (s_popup) { lv_obj_delete(s_popup); s_popup = NULL; }
    ui_unlock();
}

void ui_show_power_popup(void)
{
    ui_lock();
    if (!s_popup) {
        s_popup = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_popup, 200, 130);
        lv_obj_center(s_popup);
        lv_obj_set_style_bg_color(s_popup, lv_color_hex(0x202020), 0);
        lv_obj_set_style_border_color(s_popup, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_border_width(s_popup, 2, 0);
        lv_obj_t *t = label(s_popup, &lv_font_montserrat_20, lv_color_white());
        lv_label_set_text(t, "Power off?");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_t *h = label(s_popup, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
        lv_label_set_text(h, "key0: off   other: cancel");
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 34);
        lv_obj_t *b = lv_button_create(s_popup);
        lv_obj_set_size(b, 80, 34);
        lv_obj_set_style_bg_color(b, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_t *l = label(b, &lv_font_montserrat_14, lv_color_white());
        lv_label_set_text(l, "Off"); lv_obj_center(l);
        lv_obj_add_event_cb(b, popup_off_cb, LV_EVENT_CLICKED, NULL);
        b = lv_button_create(s_popup);
        lv_obj_set_size(b, 80, 34);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        l = label(b, &lv_font_montserrat_14, lv_color_white());
        lv_label_set_text(l, "Cancel"); lv_obj_center(l);
        lv_obj_add_event_cb(b, popup_cancel_cb, LV_EVENT_CLICKED, NULL);
        s_popup_timer = lv_timer_create(popup_timeout_cb, 8000, NULL);
        lv_timer_set_repeat_count(s_popup_timer, 1);
    }
    ui_unlock();
}

void ui_set_touch(const char *chip_name)
{
    ui_lock();
    lv_label_set_text_fmt(s_touch_lbl, "touch: %s", chip_name);
    ui_unlock();
}

/* A small ring that follows the finger, on top of every page. */
static void cursor_timer_cb(lv_timer_t *t)
{
    int16_t x, y;
    bool down = ui_port_touch_state(&x, &y);
    if (down) {
        lv_obj_set_hidden(s_cursor, false);
        lv_obj_set_pos(s_cursor, x - 12, y - 12);
        lv_label_set_text_fmt(s_touch_lbl, "touch: %d,%d", x, y);
    } else if (!lv_obj_is_hidden(s_cursor)) {
        lv_obj_set_hidden(s_cursor, true);
    }
}

void ui_create(void)
{
    ui_lock();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    build_status(scr);
    build_ride(scr);
    s_pages[0] = s_page_status;
    s_pages[1] = s_page_ride;
    s_npages = 2;

    s_cursor = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cursor);
    lv_obj_set_size(s_cursor, 24, 24);
    lv_obj_set_style_radius(s_cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_cursor, 3, 0);
    lv_obj_set_style_border_color(s_cursor, lv_palette_main(LV_PALETTE_YELLOW), 0);
    lv_obj_set_clickable(s_cursor, false);
    lv_obj_set_hidden(s_cursor, true);
    lv_timer_create(cursor_timer_cb, 30, NULL);

    build_data_pages(scr);
    menu_set_close_cb(on_menu_closed);
    lv_timer_create(data_refresh_cb, 500, NULL);
    ui_unlock();
}

void ui_set_battery(uint8_t pct, uint16_t mv)
{
    ui_lock();
    lv_arc_set_value(s_arc, pct);
    lv_label_set_text_fmt(s_arc_lbl, "%u%%\n%u mV", pct, mv);
    lv_label_set_text_fmt(s_hdr_bat, "%u%%", pct);
    s_bat_pct = pct;
    for (int i = 2; i < s_npages; i++) {
        if (s_data_bat[i]) lv_label_set_text_fmt(s_data_bat[i], "%u%%", pct);
    }
    ui_unlock();
}

void ui_set_env(int16_t temp_c100, uint32_t pressure_pa100)
{
    ui_lock();
    lv_label_set_text_fmt(s_env, "%d.%02d C   %lu.%lu hPa",
                          temp_c100 / 100, abs(temp_c100 % 100),
                          (unsigned long)(pressure_pa100 / 10000),
                          (unsigned long)((pressure_pa100 / 1000) % 10));
    lv_label_set_text_fmt(s_ride_env, "%d.%d C", temp_c100 / 100, abs(temp_c100 / 10 % 10));
    ui_unlock();
}

void ui_set_nrf(bool alive, uint8_t reason, const uint8_t fw[3])
{
    ui_lock();
    if (alive) {
        lv_label_set_text_fmt(s_nrf, "nRF ok  reason %u  fw %u.%u.%u", reason, fw[0], fw[1], fw[2]);
        lv_obj_set_style_text_color(s_nrf, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        lv_label_set_text(s_nrf, "nRF: no reply yet");
    }
    ui_unlock();
}

static void refresh_footer(void)
{
    lv_label_set_text_fmt(s_foot, "int %uK  psram %uK  bl %u%%  up %lus",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          s_bl_pct, (unsigned long)s_uptime);
}

void ui_set_backlight(uint8_t pct)
{
    s_bl_pct = pct;
    ui_lock();
    refresh_footer();
    ui_unlock();
}

static void key_reset_cb(lv_timer_t *t)
{
    lv_obj_t *box = lv_timer_get_user_data(t);
    lv_obj_set_style_bg_color(box, C_IDLE, 0);
    lv_timer_delete(t);
}

void ui_key_event(uint8_t key, uint8_t evt)
{
    ui_lock();
    if (key < 3) {
        lv_obj_t *box = s_key[key];
        if (evt == KEY_EVT_LONG_START) {
            lv_obj_set_style_bg_color(box, C_HOLD, 0);
        } else if (evt == KEY_EVT_CLICK) {
            lv_obj_set_style_bg_color(box, C_CLK, 0);
            lv_timer_create(key_reset_cb, 200, box);
        } else {
            lv_obj_set_style_bg_color(box, C_IDLE, 0);
        }
    }
    /* scroll the log */
    if (s_log_n == LOG_LINES) {
        for (int i = 1; i < LOG_LINES; i++) {
            strcpy(s_log_buf[i - 1], s_log_buf[i]);
        }
        s_log_n--;
    }
    snprintf(s_log_buf[s_log_n++], sizeof s_log_buf[0], "%6lus  key %u  evt %u",
             (unsigned long)s_uptime, key, evt);
    char all[LOG_LINES * 32];
    int n = 0;
    for (int i = 0; i < s_log_n; i++) {
        n += snprintf(all + n, sizeof all - n, "%s%s", i ? "\n" : "", s_log_buf[i]);
    }
    lv_label_set_text(s_log, all);
    ui_unlock();
}

void ui_set_gps(const gps_fix_t *g)
{
    char buf[64];
    ui_lock();
    if (g->baud == 0) {
        lv_label_set_text(s_gps, "GPS: probing baud");
        lv_label_set_text(s_ride_gps, "no GPS data");
    } else {
        const char *fix = g->valid ? (g->fix_quality == 2 ? "DGPS" : "fix") : "no fix";
        snprintf(buf, sizeof buf, "GPS %s  %u/%u sats  hdop %.1f", fix, g->sats_used, g->sats_in_view, g->hdop);
        lv_label_set_text(s_gps, buf);
        lv_obj_set_style_text_color(s_gps, g->valid ? lv_palette_main(LV_PALETTE_GREEN)
                                                    : lv_palette_main(LV_PALETTE_ORANGE), 0);
        snprintf(buf, sizeof buf, "%s  %u/%u sats  %lu sent", fix, g->sats_used, g->sats_in_view,
                 (unsigned long)g->sentences);
        lv_label_set_text(s_ride_gps, buf);
        lv_obj_set_style_text_color(s_ride_gps, g->valid ? lv_palette_main(LV_PALETTE_GREEN)
                                                         : lv_palette_main(LV_PALETTE_ORANGE), 0);
        if (g->valid) {
            lv_label_set_text_fmt(s_big, "%.1f", g->speed_kmh);
            snprintf(buf, sizeof buf, "%.5f  %.5f\nalt %.0f m  crs %.0f", g->lat, g->lon, g->alt_m, g->course_deg);
            lv_label_set_text(s_ride_pos, buf);
        }
        if (g->hh || g->mm || g->ss) {
            lv_label_set_text_fmt(s_ride_time, "%02u:%02u:%02u UTC", g->hh, g->mm, g->ss);
        }
    }
    ui_unlock();
}

/* A channel's values are shown as "--" unless it is live (see ant_live). */
void ui_set_sensors(const ant_sensors_t *v, const ant_channel_t *ch, size_t nch)
{
    char line[96], hr[8], cad[8], spd[12], pwr[8];
    uint32_t now = esp_timer_get_time() / 1000;
    bool have_hr  = ant_live(ANT_DEV_HR, ANT_DEV_HR) && v->hr_bpm;
    bool have_cad = ant_live(ANT_DEV_CADENCE, ANT_DEV_SPD_CAD);
    bool have_spd = ant_live(ANT_DEV_SPEED, ANT_DEV_SPD_CAD);
    bool have_pwr = ant_live(ANT_DEV_POWER, ANT_DEV_POWER);

    snprintf(hr, sizeof hr, have_hr ? "%u" : "--", v->hr_bpm);
    snprintf(cad, sizeof cad, have_cad ? "%.0f" : "--", v->cadence_rpm);
    snprintf(spd, sizeof spd, have_spd ? "%.1f" : "--.-", v->speed_kmh);
    int n = snprintf(line, sizeof line, "HR %s  cad %s  %s km/h", hr, cad, spd);
    if (have_pwr) {
        snprintf(pwr, sizeof pwr, "%uW", v->power_w);
        snprintf(line + n, sizeof line - n, "  %s", pwr);
    }

    ui_lock();
    lv_label_set_text(s_ride_sens, line);

    n = snprintf(line, sizeof line, "ANT");
    if (nch == 0) {
        n += snprintf(line + n, sizeof line - n, ": no paired sensors");
    }
    for (size_t i = 0; i < nch && n < (int)sizeof line - 12; i++) {
        const char *name = ch[i].dev_type == ANT_DEV_HR ? "HR" : ch[i].dev_type == ANT_DEV_CADENCE ? "cad"
                         : ch[i].dev_type == ANT_DEV_SPEED ? "spd" : ch[i].dev_type == ANT_DEV_POWER ? "pwr"
                         : ant_dev_name(ch[i].dev_type);
        bool live = ch[i].state == ANT_ST_CONNECTED && ch[i].pages && now - ch[i].last_rx_ms < ANT_LIVE_MS;
        const char *st = live ? "ok" : ch[i].state == ANT_ST_SEARCHING ? ".." : "--";
        n += snprintf(line + n, sizeof line - n, "  %s %s", name, st);
    }
    lv_label_set_text(s_ant, line);
    ui_unlock();
}

void ui_set_sd(bool mounted, const char *name, uint32_t size_mb)
{
    ui_lock();
    if (mounted) {
        lv_label_set_text_fmt(s_sd, "SD %s  %lu.%lu GB", name, (unsigned long)(size_mb / 1024),
                              (unsigned long)((size_mb % 1024) * 10 / 1024));
        lv_obj_set_style_text_color(s_sd, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        lv_label_set_text(s_sd, "SD: not mounted");
    }
    ui_unlock();
}

void ui_set_rec(bool active, uint32_t points)
{
    ui_lock();
    if (active) {
        lv_label_set_text_fmt(s_hdr_rec, LV_SYMBOL_STOP " %lu", (unsigned long)points);
        lv_label_set_text_fmt(s_ride_rec, "REC " LV_SYMBOL_STOP " %lu points", (unsigned long)points);
    } else {
        lv_label_set_text(s_hdr_rec, "");
        lv_label_set_text(s_ride_rec, points ? "stopped" : "");
    }
    ui_unlock();
}

void ui_show_usb_mode(void)
{
    ui_lock();
    show_page(-1);
    lv_obj_t *p = page(lv_screen_active());
    lv_obj_t *icon = label(p, &lv_font_montserrat_48, lv_palette_main(LV_PALETTE_BLUE));
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_t *t = label(p, &lv_font_montserrat_20, lv_color_white());
    lv_label_set_text(t, "USB storage");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_t *h = label(p, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(h, "eMMC is exposed to the host.\nEject it, then hold key 1\nto reboot.");
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 180);
    ui_unlock();
}

void ui_next_page(void)
{
    ui_lock();
    show_page((s_page_idx + 1) % s_npages);
    ui_unlock();
}

bool ui_menu_active(void)
{
    return menu_active();
}

bool ui_menu_key(uint8_t key, uint8_t evt)
{
    ui_lock();
    bool used = menu_key(key, evt);
    ui_unlock();
    return used;
}

void ui_tick(uint32_t uptime_s)
{
    s_uptime = uptime_s;
    ui_lock();
    refresh_footer();
    ui_unlock();
}
