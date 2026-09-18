#include "theme.h"

lv_style_t theme_st_bg, theme_st_text, theme_st_muted, theme_st_panel, theme_st_line;

static const theme_colors_t k_dark = {
    .bg     = LV_COLOR_MAKE(0x00, 0x00, 0x00),
    .fg     = LV_COLOR_MAKE(0xff, 0xff, 0xff),
    .muted  = LV_COLOR_MAKE(0xa0, 0xa0, 0xa0),
    .line   = LV_COLOR_MAKE(0x50, 0x50, 0x50),
    .panel  = LV_COLOR_MAKE(0x20, 0x20, 0x20),
    .sel    = LV_COLOR_MAKE(0xff, 0xd4, 0x00),
    .sel_fg = LV_COLOR_MAKE(0x00, 0x00, 0x00),
};
static const theme_colors_t k_light = {
    .bg     = LV_COLOR_MAKE(0xff, 0xff, 0xff),
    .fg     = LV_COLOR_MAKE(0x00, 0x00, 0x00),
    .muted  = LV_COLOR_MAKE(0x50, 0x50, 0x50),
    .line   = LV_COLOR_MAKE(0xc0, 0xc0, 0xc0),
    .panel  = LV_COLOR_MAKE(0xf2, 0xf2, 0xf2),
    .sel    = LV_COLOR_MAKE(0xff, 0xd4, 0x00),
    .sel_fg = LV_COLOR_MAKE(0x00, 0x00, 0x00),
};

static theme_id_t s_id;
static const theme_colors_t *s_c = &k_dark;

static void apply(void)
{
    lv_style_set_bg_color(&theme_st_bg, s_c->bg);
    lv_style_set_text_color(&theme_st_text, s_c->fg);
    lv_style_set_text_color(&theme_st_muted, s_c->muted);
    lv_style_set_bg_color(&theme_st_panel, s_c->panel);
    lv_style_set_border_color(&theme_st_panel, s_c->line);
    lv_style_set_border_color(&theme_st_line, s_c->line);
}

void theme_init(theme_id_t id)
{
    lv_style_init(&theme_st_bg);
    lv_style_init(&theme_st_text);
    lv_style_init(&theme_st_muted);
    lv_style_init(&theme_st_panel);
    lv_style_init(&theme_st_line);
    s_id = id;
    s_c = id == THEME_LIGHT ? &k_light : &k_dark;
    apply();
}

void theme_set(theme_id_t id)
{
    s_id = id;
    s_c = id == THEME_LIGHT ? &k_light : &k_dark;
    apply();
    lv_obj_report_style_change(&theme_st_bg);
    lv_obj_report_style_change(&theme_st_text);
    lv_obj_report_style_change(&theme_st_muted);
    lv_obj_report_style_change(&theme_st_panel);
    lv_obj_report_style_change(&theme_st_line);
}

theme_id_t theme_current(void) { return s_id; }
const theme_colors_t *theme_colors(void) { return s_c; }
const char *theme_name(theme_id_t id) { return id == THEME_LIGHT ? "Light" : "Dark"; }

lv_color_t theme_palette(lv_palette_t p)
{
    return s_id == THEME_LIGHT ? lv_palette_darken(p, 2) : lv_palette_main(p);
}
