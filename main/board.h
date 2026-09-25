/*
 * Board selection + everything the Magene C606, C606 Pro and C706 have in common
 * (the nRF co-processor protocol, key events, GPS power values).
 *
 * The pin maps live in board_c606.h / board_c706.h, chosen with the Kconfig
 * option C606OSS_BOARD (`idf.py -DBOARD=c706 -B build-c706 build`, see
 * tools/build_podman.sh). Both were recovered from the vendor firmware with
 * Ghidra; see docs/HARDWARE.md.
 */
#pragma once
#include "sdkconfig.h"

#if CONFIG_C606OSS_BOARD_C706
#include "board_c706.h"
#elif CONFIG_C606OSS_BOARD_C606PRO
#include "board_c606pro.h"
#elif CONFIG_C606OSS_BOARD_CC700PRO
#include "board_cc700pro.h"
#else
#include "board_c606.h"
#endif

/* ------------------------------------------------------------------ */
/* Link to the nRF co-processor ("Minor MCU"): keys, power, sensors.    */
/* Same UART, baud and framing on both boards (MidCommInit(1, 115200)). */
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
 *   E2 02 03 .. 02  LCD (re)power, sent before re-running panel init (C606)
 *   E2 02 07 .. 0X  GPS power 0/1/2
 *   E2 02 08 .. 01  factory init
 *   E2 02 09 .. 0X  LCD/touch module power (C706): 0 off, 1 on, 2 reset pulse
 *   E2 01 XX        LED / misc, XX < 0x1c                                 */
#define NRF_CTRL_LCD_POWER   0x09

/* Button event values (payload[6]); measured on hardware */
#define KEY_EVT_CLICK        1   /* short press, one frame on release */
#define KEY_EVT_LONG_START   4   /* long press: repeated by the nRF every ~250 ms while held */
#define KEY_EVT_LONG_RELEASE 5   /* release after a long press */
#define KEY_EVT_HOLD_REPEAT  6   /* synthesized by the vendor firmware, never on the wire */

/* GPS: Airoha AG3352 on UART0 on both boards, powered through the nRF:
 * E2 02 07 <val>. The baud differs (see the board file). */
#define NRF_GPS_OFF          0
#define NRF_GPS_ON           1
#define NRF_GPS_RESET        2   /* vendor "GpsHdRst" */
#define GPS_UART_NUM         0
#define GPS_UART_TX          1
#define GPS_UART_RX          0

/* On-board eMMC ("SD card"): SDMMC slot 1, 4-bit, same pins on both boards.
 * Source: MidVFSMount() */
#define SD_PIN_CLK           13
#define SD_PIN_CMD           14
#define SD_PIN_D0            16
#define SD_PIN_D1            17
#define SD_PIN_D2            18
#define SD_PIN_D3            15

/* Touch: I2C0 SDA GPIO21 / SCL GPIO12, internal pull-ups, 400 kHz on both
 * boards (InitI2CBus / I2CManagerAddDev); the controller differs. */
#define TOUCH_I2C_PORT       0
#define TOUCH_I2C_SDA        21
#define TOUCH_I2C_SCL        12
#define TOUCH_I2C_HZ         400000
#define TOUCH_ADDR_AXS15231  0x3B   /* C706: touch part of the AXS15231 panel */
#define TOUCH_ADDR_FT6X36    0x38   /* C606: FocalTech FT6336 */
#define TOUCH_ADDR_CST328    0x5A   /* other C606 HW revision: Hynitron CST328 */

/* Key roles a board may leave out: KEY_NONE never matches a key index */
#define KEY_NONE             0xFF
#ifndef KEY_PREV_PAGE
#define KEY_PREV_PAGE        KEY_NONE
#endif
#ifndef KEY_MENU_BACK
#define KEY_MENU_BACK        KEY_NONE
#endif
/* key numbers in on-screen hints: "key " KEY_STR(KEY_RIDE) */
#define KEY_STR_(x)          #x
#define KEY_STR(x)           KEY_STR_(x)
