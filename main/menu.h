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
