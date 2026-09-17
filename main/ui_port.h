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
