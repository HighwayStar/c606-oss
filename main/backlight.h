#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t backlight_init(void);
void backlight_set(uint8_t percent);
