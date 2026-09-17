/* Demo UI: a status page (battery arc, environment, key indicators, event
 * log, heap stats) and a "ride" page with big digits. Key 0 toggles pages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "board.h"
#include "ui_port.h"
#include "ui.h"

#define LOG_LINES 3

static lv_obj_t *s_page_status, *s_page_ride;
static lv_obj_t *s_hdr_bat, *s_hdr_rec, *s_arc, *s_arc_lbl, *s_env, *s_nrf, *s_gps, *s_sd, *s_log, *s_foot;
static lv_obj_t *s_key[3];
static lv_obj_t *s_big, *s_big_caption, *s_ride_env, *s_ride_gps, *s_ride_pos, *s_ride_time, *s_ride_rec;

static char s_log_buf[LOG_LINES][32];
static int s_log_n;
static uint8_t s_bl_pct = 70;
static uint32_t s_uptime;

static const lv_color_t C_BG   = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t C_HDR  = LV_COLOR_MAKE(0x10, 0x40, 0xa0);
static const lv_color_t C_IDLE = LV_COLOR_MAKE(0x30, 0x30, 0x30);
static const lv_color_t C_HOLD = LV_COLOR_MAKE(0xd0, 0x20, 0x20);
static const lv_color_t C_CLK  = LV_COLOR_MAKE(0x20, 0xa0, 0x30);

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
    lv_obj_align(s_hdr_rec, LV_ALIGN_CENTER, 20, 0);

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

    /* footer */
    s_foot = label(s_page_status, &lv_font_unscii_8, lv_palette_main(LV_PALETTE_GREY));
    lv_obj_align(s_foot, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_label_set_text(s_foot, "");
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

    s_ride_env = label(s_page_ride, &lv_font_montserrat_28, lv_palette_main(LV_PALETTE_CYAN));
    lv_label_set_text(s_ride_env, "--.- C");
    lv_obj_align(s_ride_env, LV_ALIGN_TOP_MID, 0, 230);

    s_ride_rec = label(s_page_ride, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_RED));
    lv_label_set_text(s_ride_rec, "");
    lv_obj_align(s_ride_rec, LV_ALIGN_TOP_MID, 0, 268);

    lv_obj_t *hint = label(s_page_ride, &lv_font_montserrat_14, lv_palette_main(LV_PALETTE_GREY));
    lv_label_set_text(hint, "0:page 1/2:light hold2:rec");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void ui_create(void)
{
    ui_lock();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    build_status(scr);
    build_ride(scr);
    ui_unlock();
}

void ui_set_battery(uint8_t pct, uint16_t mv)
{
    ui_lock();
    lv_arc_set_value(s_arc, pct);
    lv_label_set_text_fmt(s_arc_lbl, "%u%%\n%u mV", pct, mv);
    lv_label_set_text_fmt(s_hdr_bat, "%u%%", pct);
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

void ui_toggle_page(void)
{
    ui_lock();
    bool show_ride = lv_obj_is_hidden(s_page_ride);
    lv_obj_set_hidden(s_page_status, show_ride);
    lv_obj_set_hidden(s_page_ride, !show_ride);
    ui_unlock();
}

void ui_tick(uint32_t uptime_s)
{
    s_uptime = uptime_s;
    ui_lock();
    refresh_footer();
    ui_unlock();
}
