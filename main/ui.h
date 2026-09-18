#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gps.h"
#include "ant.h"
#include "ride.h"

typedef void (*ui_action_cb_t)(void);

/* All functions take the LVGL lock themselves; safe to call from any task. */
void ui_create(void);

void ui_set_battery(uint8_t pct, uint16_t mv);
void ui_set_env(int16_t temp_c100, uint32_t pressure_pa100);
void ui_set_nrf(bool alive, uint8_t reason, const uint8_t fw[3]);
void ui_set_backlight(uint8_t pct);
void ui_set_gps(const gps_fix_t *fix);
void ui_set_sensors(const ant_sensors_t *s, const ant_channel_t *ch, size_t nch);
void ui_set_sd(bool mounted, const char *name, uint32_t size_mb);
void ui_key_event(uint8_t key, uint8_t evt);
void ui_next_page(void);   /* key 0: idle <-> status, or the enabled data pages during a ride */
void ui_set_mode(ride_mode_t mode);
void ui_maps_changed(void);   /* after mapview_init(): adds the map page to the key-0 ring */
void ui_show_usb_mode(void);
void ui_set_usb_reboot_cb(ui_action_cb_t cb);   /* "Reboot" button of the USB page */
/* Settings menu (opened with the gear icon): route key events to it while
 * it is open. */
bool ui_menu_active(void);
bool ui_menu_key(uint8_t key, uint8_t evt);

/* Touch: chip name for the status page; callbacks for the idle page's START
 * button and the menu's System -> USB storage entry. */
void ui_set_touch(const char *chip_name);
/* on_backlight: apply config_get()->backlight (menu). */
void ui_set_actions(ui_action_cb_t on_start, ui_action_cb_t on_usb, ui_action_cb_t on_backlight);

/* Confirmation popups: power off (hold key 0) and end ride (hold key 2).
 * The key that opened it confirms, any other key / Cancel / 8 s dismisses. */
typedef enum { UI_POPUP_NONE, UI_POPUP_POWER, UI_POPUP_END_RIDE } ui_popup_t;
void ui_set_power_off_cb(ui_action_cb_t cb);
void ui_set_end_ride_cb(ui_action_cb_t cb);
void ui_show_power_popup(void);
void ui_show_end_ride_popup(void);
void ui_hide_popup(void);
ui_popup_t ui_popup_active(void);
void ui_tick(uint32_t uptime_s);   /* call ~1x per second */

/* Ride summary, shown after a ride was ended; any key click or "Done"
 * dismisses it. */
void ui_show_summary(void);
void ui_hide_summary(void);
bool ui_summary_active(void);
/* Short message over the current page (e.g. "LAP 3"), gone after 1.5 s. */
void ui_toast(const char *text);
