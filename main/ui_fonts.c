#include "ui_fonts.h"

LV_FONT_DECLARE(font_cyrillic_8);
LV_FONT_DECLARE(font_cyrillic_14);
LV_FONT_DECLARE(font_cyrillic_20);
LV_FONT_DECLARE(font_cyrillic_28);
LV_FONT_DECLARE(font_cyrillic_48);

static const lv_font_t *const k_base[] = {
    &lv_font_unscii_8, &lv_font_montserrat_14, &lv_font_montserrat_20,
    &lv_font_montserrat_28, &lv_font_montserrat_48,
};
static const lv_font_t *const k_cyrillic[] = {
    &font_cyrillic_8, &font_cyrillic_14, &font_cyrillic_20,
    &font_cyrillic_28, &font_cyrillic_48,
};
static lv_font_t s_fonts[5];

void ui_fonts_init(void)
{
    for (unsigned i = 0; i < 5; i++) {
        s_fonts[i] = *k_base[i];
        s_fonts[i].fallback = k_cyrillic[i];
    }
}

const lv_font_t *ui_font(const lv_font_t *base)
{
    for (unsigned i = 0; i < 5; i++) if (base == k_base[i]) return &s_fonts[i];
    return base;
}
