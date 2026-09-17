#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gps.h"

/* All functions take the LVGL lock themselves; safe to call from any task. */
void ui_create(void);

void ui_set_battery(uint8_t pct, uint16_t mv);
void ui_set_env(int16_t temp_c100, uint32_t pressure_pa100);
void ui_set_nrf(bool alive, uint8_t reason, const uint8_t fw[3]);
void ui_set_backlight(uint8_t pct);
void ui_set_gps(const gps_fix_t *fix);
void ui_key_event(uint8_t key, uint8_t evt);
void ui_toggle_page(void);
void ui_tick(uint32_t uptime_s);   /* call ~1x per second */
