/*
 * Magene C606 Pro board definition (pins and panel; the shared
 * nRF/GPS/eMMC/I2C constants are in board.h).
 *
 * Recovered from the vendor firmware C606P V1.723 (c606Pro_ota_0.elf, built
 * Sep 25 2025) with Ghidra. Same shell and peripherals as the C606 (ST7789
 * 240x320, FT6336/CST328 touch, backlight GPIO45, GPS at 921600, three
 * keys, 16 MB flash) but the panel sits on the C706's 8-bit i80 bus, the
 * PSRAM is octal, and the ST7789 gets a different init sequence.
 * See docs/HARDWARE.md, "C606 Pro".
 */
#pragma once

#define BOARD_NAME           "C606 Pro"

/* ------------------------------------------------------------------ */
/* Display: ST7789, 240x320, Intel-8080 **8-bit** bus (LCD_CAM)        */
/* Source: MidLcdInit() @ 0x42033db4, mg_panel_st7789_init @ 0x42034324 */
/* ------------------------------------------------------------------ */
#define LCD_PANEL_ST7789     1
#define LCD_ST7789_INIT_PRO  1    /* the Pro's init table instead of the C606's */
#define LCD_H_RES            240
#define LCD_V_RES            320

#define LCD_PIN_DC           40
#define LCD_PIN_WR           3
#define LCD_PIN_RD           39   /* configured as output, driven high */
#define LCD_PIN_CS           2
#define LCD_PIN_RST          -1   /* not wired to the ESP32 */

/* data_gpio_nums[0..7] in the order the vendor passes them (bus_width 8) */
#define LCD_PIN_D0           4
#define LCD_PIN_D1           38
#define LCD_PIN_D2           5
#define LCD_PIN_D3           48
#define LCD_PIN_D4           6
#define LCD_PIN_D5           47
#define LCD_PIN_D6           7
#define LCD_PIN_D7           11
#define LCD_PIN_D8           -1
#define LCD_PIN_D9           -1
#define LCD_PIN_D10          -1
#define LCD_PIN_D11          -1
#define LCD_PIN_D12          -1
#define LCD_PIN_D13          -1
#define LCD_PIN_D14          -1
#define LCD_PIN_D15          -1

#define LCD_BUS_WIDTH        8
#define LCD_PCLK_HZ          (15 * 1000 * 1000)
#define LCD_CMD_BITS         8
#define LCD_PARAM_BITS       8
#define LCD_TRANS_QUEUE_DEPTH 10
/* 8-bit bus: two bytes per pixel, high byte first (vendor LVGL 8 build has
 * LV_COLOR_16_SWAP = 1, its palette table is byte-swapped); we render
 * little-endian RGB565 and let the DMA swap. */
#define LCD_SWAP_COLOR_BYTES 1
/* vendor: 28800 bytes = 240 px * 60 lines * 2 bytes per LVGL draw buffer */
#define LCD_BUF_LINES        60
#define LCD_MAX_TRANSFER     (LCD_H_RES * LCD_BUF_LINES * 2)

/* ------------------------------------------------------------------ */
/* Backlight: LEDC PWM.  Source: MidLcdPwmInit() @ 0x42033ca0           */
/* ------------------------------------------------------------------ */
#define BL_PIN               45
#define BL_LEDC_TIMER        0
#define BL_LEDC_CHANNEL      0
#define BL_PWM_FREQ_HZ       20000
#define BL_PWM_RES_BITS      10

/* GPS: AppDevInit opens UART0 at 921600 like the C606 */
#define GPS_UART_BAUD        921600

/* Touch: FT6336 at 0x38, then CST328 at 0x5A (FUN_420341f0) */
#define TOUCH_HAS_AXS15231   0
#define TOUCH_HAS_FT6X36     1
#define TOUCH_HAS_CST328     1

/* ------------------------------------------------------------------ */
/* Keys: three (vendor A, B, C = nRF index 0..2; the D/E entries of the  */
/* vendor key map are empty). Same roles as the C606 build.              */
/* ------------------------------------------------------------------ */
#define NUM_KEYS             3
#define KEY_POWER            0
#define KEY_RIDE             2
#define KEY_LAP              1
#define KEY_NEXT_PAGE        0
#define KEY_USB_EXIT         1
#define KEY_MENU_UP          2
#define KEY_MENU_DOWN        1
#define KEY_MENU_SELECT      0
