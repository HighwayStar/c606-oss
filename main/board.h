/*
 * Magene C606 board definition.
 *
 * Every value here was recovered from the vendor firmware
 * (C606 v1.711, ota_0-606-1.711.elf) with Ghidra. Function names in
 * comments refer to the vendor's own names found in log strings.
 * See docs/HARDWARE.md for the analysis notes.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Display: ST7789, 240x320, Intel-8080 16-bit parallel bus (LCD_CAM)   */
/* Source: MidLcdInit() in Modules/Middlewares/MidLcd/MidLcd.c          */
/* ------------------------------------------------------------------ */
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
/* vendor: 28800 bytes = 240 px * 60 lines * 2 bytes per LVGL draw buffer */
#define LCD_VENDOR_MAX_TRANSFER 28800

/* ------------------------------------------------------------------ */
/* Backlight: LEDC PWM.  Source: MidLcdPwmInit()                        */
/* ------------------------------------------------------------------ */
#define BL_PIN               45
#define BL_LEDC_TIMER        0
#define BL_LEDC_CHANNEL      0
#define BL_PWM_FREQ_HZ       20000
#define BL_PWM_RES_BITS      10

/* ------------------------------------------------------------------ */
/* Link to the nRF co-processor ("Minor MCU"): keys, power, sensors.    */
/* Source: MidCommInit(1, 115200, ...) -> UART2, uart_set_pin(2,42,41)   */
/* ------------------------------------------------------------------ */
#define NRF_UART_NUM         2
#define NRF_UART_TX          42
#define NRF_UART_RX          41
#define NRF_UART_BAUD        115200

/* Frame layout, both directions (see nrf_link.c):
 *   [0] 0xA5            sync
 *   [1] N               = 4 + payload length; total frame = N + 4
 *   [2] 0x6F            'o' constant
 *   [3] 0xF1 (ESP->nRF) / any (nRF->ESP), not checked by vendor parser
 *   [4] type            1..5 (ESP uses 1 = query, 2 = set)
 *   [5] cmd             0x10 = "system" commands (keys, power, GPS, LED)
 *   [6..6+len-1]        payload
 *   [N+2..N+3]          CRC16 (little endian) over bytes [0..N+1]
 */
#define NRF_SYNC             0xA5
#define NRF_MARK             0x6F
#define NRF_DIR_ESP_TO_NRF   0xF1
#define NRF_TYPE_QUERY       1
#define NRF_TYPE_SET         2
#define NRF_CMD_SYS          0x10
#define NRF_MAX_FRAME        0x88   /* vendor rejects N >= 0x85 */

/* cmd 0x10 payload[0] sub-commands */
#define NRF_SYS_BUTTON       0x49   /* nRF -> ESP: [1]=key idx, [5]=?, [6]=event */
#define NRF_SYS_CTRL         0xE2   /* ESP -> nRF: [1]=group, [2]=id, [5]=value */
/*   E2 02 00 .. 01  SendPowerOnCmd   (vendor sends this in INIT_QUERY until
 *                                     the nRF answers with E2 02 .. reason)
 *   E2 02 00 .. 00  SendPowerOffCmd
 *   E2 02 03 .. 02  LCD (re)power, sent before re-running panel init
 *   E2 02 07 .. 0X  GPS power 0/1/2
 *   E2 02 08 .. 01  factory init
 *   E2 01 XX        LED / misc, XX < 0x1c                                 */

/* Button event values seen in KeyQueueReceive() */
#define KEY_EVT_LONG_START   4   /* "Long Press Start" */
#define KEY_EVT_HOLD_REPEAT  6   /* synthesized by vendor every 300 ms while held */

/* ------------------------------------------------------------------ */
/* Other peripherals (not used by the PoC, documented for later)         */
/* ------------------------------------------------------------------ */
/* GPS: UART0 @ 921600, TX=GPIO1, RX=GPIO0 (MidCommInit(0, 921600, ...)) */
#define GPS_UART_NUM         0
#define GPS_UART_TX          1
#define GPS_UART_RX          0
#define GPS_UART_BAUD        921600

/* SD card: SDMMC slot, 4-bit. Source: MidVFSMount() */
#define SD_PIN_CLK           13
#define SD_PIN_CMD           14
#define SD_PIN_D0            16
#define SD_PIN_D1            17
#define SD_PIN_D2            18
#define SD_PIN_D3            15

/* Touch (C606 Pro / HW variant): I2C, controller probed at 0x38 then 0x5A */
#define TOUCH_I2C_ADDR_A     0x38
#define TOUCH_I2C_ADDR_B     0x5A

/* On HW variant (NVS "Res1Page11" byte3 == 2) the vendor drives GPIO43/44
 * (default UART0 pins) high as plain outputs during LCD init. */
