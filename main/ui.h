#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gps.h"
#include "ant.h"

/* All functions take the LVGL lock themselves; safe to call from any task. */
void ui_create(void);

void ui_set_battery(uint8_t pct, uint16_t mv);
void ui_set_env(int16_t temp_c100, uint32_t pressure_pa100);
void ui_set_nrf(bool alive, uint8_t reason, const uint8_t fw[3]);
void ui_set_backlight(uint8_t pct);
void ui_set_gps(const gps_fix_t *fix);
void ui_set_sensors(const ant_sensors_t *s, const ant_channel_t *ch, size_t nch);
void ui_set_sd(bool mounted, const char *name, uint32_t size_mb);
void ui_set_rec(bool active, uint32_t points);
void ui_key_event(uint8_t key, uint8_t evt);
void ui_next_page(void);   /* status -> ride -> enabled data pages -> ... */
/* Settings menu (opened with the gear icon): route key events to it while
 * it is open. */
bool ui_menu_active(void);
bool ui_menu_key(uint8_t key, uint8_t evt);
void ui_show_usb_mode(void);

/* Touch: chip name for the status page; callbacks for the on-screen buttons. */
void ui_set_touch(const char *chip_name);
typedef void (*ui_action_cb_t)(void);
void ui_set_actions(ui_action_cb_t on_rec, ui_action_cb_t on_usb);

/* Power-off confirmation (hold key 0). Key 0 click / "Off" confirms,
 * any other key / "Cancel" / 8 s timeout dismisses. */
void ui_set_power_off_cb(ui_action_cb_t cb);
void ui_show_power_popup(void);
void ui_hide_power_popup(void);
bool ui_power_popup_active(void);
void ui_tick(uint32_t uptime_s);   /* call ~1x per second */
