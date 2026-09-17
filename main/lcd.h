#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* RGB565, native word order (the panel is on a 16-bit bus, so no byte swap). */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BLACK   RGB565(0, 0, 0)
#define C_WHITE   RGB565(255, 255, 255)
#define C_RED     RGB565(255, 0, 0)
#define C_GREEN   RGB565(0, 255, 0)
#define C_BLUE    RGB565(0, 0, 255)
#define C_YELLOW  RGB565(255, 255, 0)
#define C_CYAN    RGB565(0, 255, 255)
#define C_GREY    RGB565(96, 96, 96)
#define C_DGREY   RGB565(32, 32, 32)
#define C_ORANGE  RGB565(255, 140, 0)

esp_err_t lcd_init(void);

/* All drawing goes to an in-RAM framebuffer; lcd_flush() pushes it out. */
void lcd_fill(uint16_t color);
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void lcd_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
/* Text grid helpers: column/row in units of the built-in font. */
void lcd_text_rc(int col, int row, const char *s, uint16_t fg, uint16_t bg);
int  lcd_text_cols(void);
int  lcd_text_rows(void);

/* Blocking: sends the whole framebuffer and waits for DMA completion. */
void lcd_flush(void);

/* Send display on/off (0x29 / 0x28). */
void lcd_display_on(bool on);
