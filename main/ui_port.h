#pragma once
#include "esp_err.h"

esp_err_t ui_port_init(void);

/* Hold this around any lv_* call made outside the LVGL task. Recursive. */
void ui_lock(void);
void ui_unlock(void);
