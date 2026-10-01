#pragma once
#include "esp_err.h"

esp_err_t ui_port_init(void);

/* Hold this around any lv_* call made outside the LVGL task. Recursive. */
void ui_lock(void);
void ui_unlock(void);

/* Last raw touch point (screen coordinates) and whether a finger is down. */
#include <stdint.h>
#include <stdbool.h>
bool ui_port_touch_state(int16_t *x, int16_t *y);
/* Simulate a finger at (x, y) for hold_ms (developer console). */
void ui_port_inject_touch(int16_t x, int16_t y, uint32_t hold_ms);
void ui_port_inject_swipe(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms);
