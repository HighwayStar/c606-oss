/*
 * LCD on the ESP32-S3 LCD_CAM i80 bus.
 *
 *   C606: ST7789 240x320 on a 16-bit bus. The vendor firmware uses esp_lcd's
 *         i80 bus + panel_io with its own copy of the ST7789 panel driver
 *         ("mg_esp_lcd_panel_st7789.c"); we replay its init sequence verbatim.
 *   C706: AXS15231-family 320x480 on an 8-bit bus ("mg_esp_lcd_panel_axs1523").
 *         The panel initialises itself: the vendor only pulses its reset
 *         through the nRF (E2 02 09 .. 02), sends INVOFF and starts drawing.
 *
 * Either way the panel is driven with raw CASET/RASET/RAMWR. LVGL renders
 * into partial buffers and hands them to lcd_draw_bitmap() (see ui_port.c).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_attr.h"

#include "board.h"
#include "lcd.h"
#include "nrf_link.h"

static const char *TAG = "lcd";

/* ST7789: set to 0x08 if red and blue come out swapped on your unit. Vendor uses 0. */
#ifndef LCD_MADCTL
#define LCD_MADCTL 0x00
#endif

static esp_lcd_i80_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_io_color_trans_done_cb_t s_done_cb;
static void *s_done_ctx;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    return s_done_cb ? s_done_cb(io, ev, s_done_ctx) : false;
}

void lcd_set_done_cb(esp_lcd_panel_io_color_trans_done_cb_t cb, void *ctx)
{
    s_done_ctx = ctx;
    s_done_cb = cb;
}

static void cmd(uint8_t c, const void *p, size_t n)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, c, p, n));
}

#if LCD_PANEL_ST7789
/* Exact sequence from vendor mg_panel_st7789_init(), incl. the 1 ms gaps. */
static void panel_init(void)
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
#define PANEL_NAME "ST7789"
#elif LCD_PANEL_AXS15231
/* Vendor MidLcdInit(): esp_lcd_panel_reset() = mg_panel_axs1523_reset(),
 * which sends E2 02 09 00 00 02 00 00 to the nRF and waits 150 ms;
 * esp_lcd_panel_init() only logs; then invert_color(false), set_gap(0,0).
 * No MADCTL/COLMOD - the panel comes up configured on its own. */
static void panel_init(void)
{
    cmd(0x20, NULL, 0);                 /* INVOFF */
}
#define PANEL_NAME "AXS15231"
#else
#error "board.h must define LCD_PANEL_ST7789 or LCD_PANEL_AXS15231"
#endif

esp_err_t lcd_init(void)
{
#if LCD_PANEL_AXS15231
    /* the module's reset line is on the nRF: pulse it before touching the bus */
    if (nrf_link_uart_init() == ESP_OK) {
        nrf_link_send_lcd_power(2);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
#endif
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
        .max_transfer_bytes = LCD_MAX_TRANSFER,   /* one LVGL draw buffer */
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
        /* vendor flags = 0: cs active low, pclk active pos; the byte swap
         * replaces the vendor's LV_COLOR_16_SWAP on the 8-bit bus */
        .flags.swap_color_bytes = LCD_SWAP_COLOR_BYTES,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(s_bus, &io, &s_io), TAG, "panel io");

    panel_init();
    ESP_LOGI(TAG, PANEL_NAME " %dx%d on i80x%d @ %d MHz ready",
             LCD_H_RES, LCD_V_RES, LCD_BUS_WIDTH, LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

void lcd_display_on(bool on)
{
    cmd(on ? 0x29 : 0x28, NULL, 0);
}

/* ---------------------------------------------------------------- */
/* Drawing                                                           */
/* ---------------------------------------------------------------- */

void lcd_draw_bitmap(int x1, int y1, int x2, int y2, const void *px)
{
    /* vendor mg_panel_*_draw_bitmap(): CASET, RASET, then RAMWR+color */
    const uint8_t caset[] = {x1 >> 8, x1 & 0xFF, (x2 - 1) >> 8, (x2 - 1) & 0xFF};
    const uint8_t raset[] = {y1 >> 8, y1 & 0xFF, (y2 - 1) >> 8, (y2 - 1) & 0xFF};
    cmd(0x2A, caset, sizeof caset);
    cmd(0x2B, raset, sizeof raset);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_io, 0x2C, px, (size_t)(x2 - x1) * (y2 - y1) * 2));
}
