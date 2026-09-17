#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"

esp_err_t lcd_init(void);

/* Called from ISR context when a lcd_draw_bitmap() transfer has completed. */
void lcd_set_done_cb(esp_lcd_panel_io_color_trans_done_cb_t cb, void *ctx);

/* Push an RGB565 bitmap to [x1,x2) x [y1,y2). Asynchronous (DMA); `px` must
 * stay valid and unmodified until the done callback fires. */
void lcd_draw_bitmap(int x1, int y1, int x2, int y2, const void *px);

/* Send display on/off (0x29 / 0x28). */
void lcd_display_on(bool on);
