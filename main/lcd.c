/*
 * ST7789 on the ESP32-S3 LCD_CAM i80 (16-bit) bus.
 *
 * The vendor firmware uses esp_lcd's i80 bus + panel_io with its own copy of
 * the ST7789 panel driver ("mg_esp_lcd_panel_st7789.c"). We use the same bus
 * and panel-io drivers from ESP-IDF and replay the vendor's init sequence
 * verbatim, then drive the panel with raw CASET/RASET/RAMWR from a full-frame
 * RGB565 framebuffer in internal (DMA-capable) RAM.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_attr.h"

#include "board.h"
#include "lcd.h"
#include "font.h"

static const char *TAG = "lcd";

/* Set to 0x08 if red and blue come out swapped on your unit. Vendor uses 0. */
#ifndef LCD_MADCTL
#define LCD_MADCTL 0x00
#endif

#define FB_PIXELS (LCD_H_RES * LCD_V_RES)
#define FB_BYTES  (FB_PIXELS * 2)

static esp_lcd_i80_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_fb;
static SemaphoreHandle_t s_done;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

static void cmd(uint8_t c, const void *p, size_t n)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, c, p, n));
}

/* Exact sequence from vendor mg_panel_st7789_init(), incl. the 1 ms gaps. */
static void st7789_vendor_init(void)
{
    const uint8_t madctl = LCD_MADCTL;
    const uint8_t colmod = 0x55;                              /* 16 bpp */
    const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33}; /* B2 */
    const uint8_t gctrl    = 0x74;                            /* B7 */
    const uint8_t vcoms    = 0x1E;                            /* BB */
    const uint8_t lcmctrl  = 0x2C;                            /* C0 */
    const uint8_t vdvvrhen = 0x01;                            /* C2 */
    const uint8_t vrhs     = 0x10;                            /* C3 */
    const uint8_t vdvs     = 0x20;                            /* C4 */
    const uint8_t frctrl2  = 0x0F;                            /* C6 */
    const uint8_t pwctrl1[] = {0xA4, 0xA1};                   /* D0 */
    const uint8_t pvgam[] = {0xF0, 0x06, 0x0B, 0x06, 0x07, 0x25, 0x34,
                             0x44, 0x4A, 0x38, 0x14, 0x13, 0x2E, 0x34}; /* E0 */
    const uint8_t nvgam[] = {0xF0, 0x0C, 0x10, 0x0A, 0x09, 0x06, 0x33,
                             0x43, 0x49, 0x36, 0x12, 0x14, 0x2A, 0x32}; /* E1 */
    const uint8_t e9[] = {0x11, 0x11, 0x03};                  /* E9 */

    /* mg_panel_st7789_reset(): no reset GPIO -> SWRESET + 20 ms */
    cmd(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    cmd(0x11, NULL, 0);                 vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x36, &madctl, 1);              vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0x3A, &colmod, 1);              vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xB2, porctrl, sizeof porctrl); vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xB7, &gctrl, 1);               vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xBB, &vcoms, 1);               vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xC0, &lcmctrl, 1);             vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xC2, &vdvvrhen, 1);            vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xC3, &vrhs, 1);                vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xC4, &vdvs, 1);                vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xC6, &frctrl2, 1);             vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xD0, pwctrl1, sizeof pwctrl1); vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xE0, pvgam, sizeof pvgam);     vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xE1, nvgam, sizeof nvgam);     vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0xE9, e9, sizeof e9);           vTaskDelay(pdMS_TO_TICKS(1));
    cmd(0x21, NULL, 0);                 vTaskDelay(pdMS_TO_TICKS(1)); /* INVON */
    cmd(0x29, NULL, 0);                 vTaskDelay(pdMS_TO_TICKS(1)); /* DISPON */
    cmd(0x2C, NULL, 0);                 vTaskDelay(pdMS_TO_TICKS(1)); /* RAMWR */
}

esp_err_t lcd_init(void)
{
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "sem");

    s_fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "framebuffer (%d bytes)", FB_BYTES);
    memset(s_fb, 0, FB_BYTES);

    /* Vendor: RD is a plain output held high (we never read from the panel). */
    gpio_config_t rd = {
        .pin_bit_mask = 1ULL << LCD_PIN_RD,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rd), TAG, "rd gpio");
    gpio_set_level(LCD_PIN_RD, 1);

    esp_lcd_i80_bus_config_t bus = {
        .dc_gpio_num = LCD_PIN_DC,
        .wr_gpio_num = LCD_PIN_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,           /* vendor: PLL_F160M */
        .data_gpio_nums = {
            LCD_PIN_D0,  LCD_PIN_D1,  LCD_PIN_D2,  LCD_PIN_D3,
            LCD_PIN_D4,  LCD_PIN_D5,  LCD_PIN_D6,  LCD_PIN_D7,
            LCD_PIN_D8,  LCD_PIN_D9,  LCD_PIN_D10, LCD_PIN_D11,
            LCD_PIN_D12, LCD_PIN_D13, LCD_PIN_D14, LCD_PIN_D15,
        },
        .bus_width = LCD_BUS_WIDTH,
        .max_transfer_bytes = FB_BYTES,           /* we push whole frames */
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus, &s_bus), TAG, "i80 bus");

    esp_lcd_panel_io_i80_config_t io = {
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = LCD_TRANS_QUEUE_DEPTH,
        .on_color_trans_done = on_color_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        /* vendor flags = 0: cs active low, no swap, pclk active pos */
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(s_bus, &io, &s_io), TAG, "panel io");

    st7789_vendor_init();
    ESP_LOGI(TAG, "ST7789 %dx%d on i80x%d @ %d MHz ready",
             LCD_H_RES, LCD_V_RES, LCD_BUS_WIDTH, LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

void lcd_display_on(bool on)
{
    cmd(on ? 0x29 : 0x28, NULL, 0);
}

/* ---------------------------------------------------------------- */
/* Framebuffer drawing                                               */
/* ---------------------------------------------------------------- */

void lcd_fill(uint16_t color)
{
    for (int i = 0; i < FB_PIXELS; i++) {
        s_fb[i] = color;
    }
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = &s_fb[yy * LCD_H_RES + x];
        for (int xx = 0; xx < w; xx++) {
            row[xx] = color;
        }
    }
}

static void draw_glyph(int x, int y, char ch, uint16_t fg, uint16_t bg)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    const uint8_t *g = font_glyphs[ch - FONT_FIRST];
    for (int gy = 0; gy < FONT_H; gy++) {
        int py = y + gy;
        if (py < 0 || py >= LCD_V_RES) continue;
        for (int gx = 0; gx < FONT_W; gx++) {
            int px = x + gx;
            if (px < 0 || px >= LCD_H_RES) continue;
            bool on = g[gy * FONT_BPR + (gx >> 3)] & (0x80 >> (gx & 7));
            s_fb[py * LCD_H_RES + px] = on ? fg : bg;
        }
    }
}

void lcd_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    for (; *s; s++, x += FONT_W) {
        draw_glyph(x, y, *s, fg, bg);
    }
}

void lcd_text_rc(int col, int row, const char *s, uint16_t fg, uint16_t bg)
{
    lcd_draw_text(col * FONT_W, row * FONT_H, s, fg, bg);
}

int lcd_text_cols(void) { return LCD_H_RES / FONT_W; }
int lcd_text_rows(void) { return LCD_V_RES / FONT_H; }

/* ---------------------------------------------------------------- */
/* Flush                                                             */
/* ---------------------------------------------------------------- */

void lcd_flush(void)
{
    /* vendor mg_panel_st7789_draw_bitmap(): CASET, RASET, then RAMWR+color */
    const uint8_t caset[] = {0, 0, (LCD_H_RES - 1) >> 8, (LCD_H_RES - 1) & 0xFF};
    const uint8_t raset[] = {0, 0, (LCD_V_RES - 1) >> 8, (LCD_V_RES - 1) & 0xFF};
    cmd(0x2A, caset, sizeof caset);
    cmd(0x2B, raset, sizeof raset);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io, 0x2C, s_fb, FB_BYTES));
    /* Block until DMA has finished reading the framebuffer. */
    xSemaphoreTake(s_done, pdMS_TO_TICKS(1000));
}
