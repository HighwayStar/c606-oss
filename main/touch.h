#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum { TOUCH_NONE = 0, TOUCH_FT6X36, TOUCH_CST328 } touch_chip_t;

esp_err_t touch_init(void);
touch_chip_t touch_chip(void);
const char *touch_chip_name(void);

/* Polls the controller. Returns true while a finger is down; x/y are raw
 * controller coordinates (0..239 / 0..319 expected, orientation TBD). */
bool touch_read(uint16_t *x, uint16_t *y);
