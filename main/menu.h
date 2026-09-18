#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Settings menu (pages, layouts, fields, time zone, ...). Drawn on the top
 * layer above the pages. Touch and keys both work: key 2 = up, key 1 =
 * down, key 0 = select (on the back arrow: back). All functions expect the
 * LVGL lock to be held. */

void menu_open(void);
void menu_close(void);
bool menu_active(void);
/* Feed key events while the menu is open; returns true when consumed. */
bool menu_key(uint8_t key, uint8_t evt);
/* Called after the menu closes; the configuration may have changed. */
void menu_set_close_cb(void (*cb)(void));
/* System actions (the menu closes first, then the callback runs). */
/* MENU_ACTION_BACKLIGHT runs on every step of the backlight screen (read
 * config_get()->backlight), the others after the menu has closed. */
typedef enum { MENU_ACTION_USB, MENU_ACTION_POWER_OFF, MENU_ACTION_BACKLIGHT, MENU_ACTION_COUNT } menu_action_t;
void menu_set_action_cb(menu_action_t a, void (*cb)(void));
