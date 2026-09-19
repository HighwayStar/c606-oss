/*
 * Magene C706 board definition (pins and panel; the shared nRF/GPS/eMMC/I2C
 * constants are in board.h).
 *
 * Recovered from the vendor firmware C706 V1.729 (ota_0-706.elf) with
 * Ghidra, cross-checked with the device's own logs (LOG/<date>.log on the eMMC).
 * NOT yet verified on hardware - see docs/HARDWARE.md, "C706".
 */
#pragma once

#define BOARD_NAME           "C706"

/* ------------------------------------------------------------------ */
/* Display: AXS15231-family panel ("axs1523" in the vendor's driver),   */
/* 320x480, 16 bpp, Intel-8080 **8-bit** bus on LCD_CAM.                */
/* Source: MidLcdInit() @ 0x42033f5c, mg_esp_lcd_new_panel_axs1523.     */
/* The panel initialises itself: the vendor sends no init sequence, only */
/* a reset through the nRF (E2 02 09 .. 02) and, after that, CASET/RASET */
/* /RAMWR windows that must be 4-pixel aligned (the driver warns if not). */
/* ------------------------------------------------------------------ */
#define LCD_PANEL_AXS15231   1
#define LCD_H_RES            320
#define LCD_V_RES            480
#define LCD_ALIGN_PX         4    /* vendor mg_panel_axs1523_draw_bitmap(): x/y start/end % 4 == 0 */

#define LCD_PIN_DC           40
#define LCD_PIN_WR           3
#define LCD_PIN_RD           39   /* configured as output, driven high */
#define LCD_PIN_CS           2
#define LCD_PIN_RST          -1   /* through the nRF */

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
#define LCD_PCLK_HZ          (20 * 1000 * 1000)
#define LCD_CMD_BITS         8
#define LCD_PARAM_BITS       8
#define LCD_TRANS_QUEUE_DEPTH 10
/* On an 8-bit bus a pixel goes out as two bytes, high byte first. The
 * vendor's LVGL 8 build renders big-endian (LV_COLOR_16_SWAP = 1, its
 * palette table is byte-swapped in the image) with bus flags 0; we render
 * little-endian RGB565 and let the DMA swap. */
#define LCD_SWAP_COLOR_BYTES 1
/* vendor mg_lvgl_port: two buffers of hres*vres/10 px = 30720 bytes each */
#define LCD_BUF_LINES        48
#define LCD_MAX_TRANSFER     (LCD_H_RES * LCD_BUF_LINES * 2)

/* ------------------------------------------------------------------ */
/* Backlight: LEDC PWM.  Source: MidLcdPwmInit() @ 0x42033e4c           */
/* ------------------------------------------------------------------ */
#define BL_PIN               10
#define BL_LEDC_TIMER        0
#define BL_LEDC_CHANNEL      0
#define BL_PWM_FREQ_HZ       20000
#define BL_PWM_RES_BITS      10

/* GPS: AppDevInit opens UART0 at 115200 (the vendor switches to the rate
 * stored for the chip later); gps.c probes the usual rates anyway. */
#define GPS_UART_BAUD        115200

/* Touch: the AXS15231's own controller at 0x3B (vendor probes it first,
 * then FT6336 / CST328 like the C606). */
#define TOUCH_HAS_AXS15231   1
#define TOUCH_HAS_FT6X36     1
#define TOUCH_HAS_CST328     1

/* ------------------------------------------------------------------ */
/* Keys: five, nRF index 0..4 = vendor A..E. Roles follow the vendor's    */
/* key map (KeyMapInfo, scene 1 = riding, scene 0 = menus):              */
/*   A click lap / hold power-off popup, B click bell / hold intercom     */
/*   ("the button at the lower left corner"), C click start-pause / hold */
/*   end ride, D next page, E previous page; menus: A back, C select,    */
/*   D/E next/previous item.                                             */
/* ------------------------------------------------------------------ */
#define NUM_KEYS             5
#define KEY_POWER            0
#define KEY_RIDE             2
#define KEY_LAP              0   /* click (the hold is the power popup) */
#define KEY_NEXT_PAGE        3
#define KEY_PREV_PAGE        4
#define KEY_USB_EXIT         1   /* hold in USB storage mode: reboot */
#define KEY_MENU_UP          4
#define KEY_MENU_DOWN        3
#define KEY_MENU_SELECT      2
#define KEY_MENU_BACK        0
/* key 1 (B, the vendor's bell / intercom key) is unused for now */

/* The vendor drives GPIO43/44 high as plain outputs during LCD init on
 * the same NVS HW-variant flag as the C606. */
