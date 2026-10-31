#pragma once
#include "lvgl.h"

/* Initialize once before starting LVGL tasks. Latin/icons retain the
 * upstream fonts; only missing Cyrillic/punctuation uses DejaVu Sans. */
void ui_fonts_init(void);
const lv_font_t *ui_font(const lv_font_t *base);
