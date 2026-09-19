/*
 * Magene C606 board definition (pins and panel; the shared nRF/GPS/eMMC/I2C
 * constants are in board.h).
 *
 * Every value here was recovered from the vendor firmware
 * (C606 v1.711, ota_0-606-1.711.elf) with Ghidra. Function names in
 * comments refer to the vendor's own names found in log strings.
 * See docs/HARDWARE.md for the analysis notes.
 */
#pragma once

#define BOARD_NAME           "C606"

/* ------------------------------------------------------------------ */
/* Display: ST7789, 240x320, Intel-8080 16-bit parallel bus (LCD_CAM)   */
/* Source: MidLcdInit() in Modules/Middlewares/MidLcd/MidLcd.c          */
/* ------------------------------------------------------------------ */
#define LCD_PANEL_ST7789     1
#define LCD_H_RES            240
#define LCD_V_RES            320

#define LCD_PIN_DC           40
#define LCD_PIN_WR           3
#define LCD_PIN_RD           39   /* configured as output, driven high */
#define LCD_PIN_CS           2
#define LCD_PIN_RST          -1   /* not wired to the ESP32 (nRF side) */

/* data_gpio_nums[0..15] in the order the vendor passes them */
#define LCD_PIN_D0           4
#define LCD_PIN_D1           38
#define LCD_PIN_D2           5
#define LCD_PIN_D3           37
#define LCD_PIN_D4           6
#define LCD_PIN_D5           36
#define LCD_PIN_D6           7
#define LCD_PIN_D7           35
#define LCD_PIN_D8           8
#define LCD_PIN_D9           34
#define LCD_PIN_D10          9
#define LCD_PIN_D11          33
#define LCD_PIN_D12          10
#define LCD_PIN_D13          47
#define LCD_PIN_D14          11
#define LCD_PIN_D15          48

#define LCD_BUS_WIDTH        16
#define LCD_PCLK_HZ          (15 * 1000 * 1000)
#define LCD_CMD_BITS         8
#define LCD_PARAM_BITS       8
#define LCD_TRANS_QUEUE_DEPTH 10
/* a 16-bit bus moves one pixel per clock: LVGL's little-endian RGB565 goes
 * out as-is (the vendor's LVGL 8 build has LV_COLOR_16_SWAP = 0) */
#define LCD_SWAP_COLOR_BYTES 0
/* vendor: 28800 bytes = 240 px * 60 lines * 2 bytes per LVGL draw buffer */
#define LCD_BUF_LINES        60
#define LCD_MAX_TRANSFER     (LCD_H_RES * LCD_BUF_LINES * 2)

/* ------------------------------------------------------------------ */
/* Backlight: LEDC PWM.  Source: MidLcdPwmInit()                        */
/* ------------------------------------------------------------------ */
#define BL_PIN               45
#define BL_LEDC_TIMER        0
#define BL_LEDC_CHANNEL      0
#define BL_PWM_FREQ_HZ       20000
#define BL_PWM_RES_BITS      10

/* GPS: MidCommInit(0, 921600, ...) - the module's configured rate; gps.c
 * probes the other usual rates if nothing decodes. */
#define GPS_UART_BAUD        921600

/* Touch controllers seen on C606 units (probed in this order by the vendor,
 * FUN_42031b14): FT6336 at 0x38, then CST328 at 0x5A. No RST/INT GPIO. */
#define TOUCH_HAS_AXS15231   0
#define TOUCH_HAS_FT6X36     1
#define TOUCH_HAS_CST328     1

/* ------------------------------------------------------------------ */
/* Keys: three, reported by the nRF as index 0..2 (vendor A, B, C).      */
/* Roles used by main.c / ui.c / menu.c.                                 */
/* ------------------------------------------------------------------ */
#define NUM_KEYS             3
#define KEY_POWER            0   /* hold: power-off popup (the popup's own key confirms) */
#define KEY_RIDE             2   /* click: start / pause / resume, hold: end ride */
#define KEY_LAP              1   /* click during a ride: manual lap */
#define KEY_NEXT_PAGE        0   /* click: next page */
/* KEY_PREV_PAGE: none */
#define KEY_USB_EXIT         1   /* hold in USB storage mode: reboot */
#define KEY_MENU_UP          2
#define KEY_MENU_DOWN        1
#define KEY_MENU_SELECT      0   /* on the back arrow: back */
/* KEY_MENU_BACK: none (select the back arrow) */

/* On HW variant (NVS "Res1Page11" byte3 == 2) the vendor drives GPIO43/44
 * (default UART0 pins) high as plain outputs during LCD init. */
