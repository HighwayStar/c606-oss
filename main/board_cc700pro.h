/*
 * Geoid CC700 Pro board definition: the C606 Pro's pins, 8-bit panel bus,
 * octal PSRAM and keys (board_c606pro.h), with the plain C606's ST7789 init
 * and GPIO43/44 driven high.
 *
 * From its vendor firmware CC700Pro_V1.712 (Apr 2 2026) and the running
 * device: the pin map read live from the GPIO matrix / IO_MUX over JTAG
 * matches board_c606pro.h + board.h; mg_panel_st7789_init() @ 0x420322b8
 * sends the C606 sequence byte for byte (constants at 0x3c373718, the C606
 * Pro's gamma tables are not in the image). See the README, "Geoid CC700 Pro".
 */
#pragma once

#define BOARD_NAME           "CC700 Pro"
#define LCD_ST7789_INIT_PRO  0    /* the C606's init table */
#include "board_c606pro.h"

/* The vendor firmware leaves GPIO43/44 (default UART0 pins) as plain
 * outputs driven high; mirror that before LCD init. The C606 / C706 vendor
 * builds do the same only on an NVS HW-variant flag. */
#define LCD_AUX_HIGH_MASK    ((1ULL << 43) | (1ULL << 44))
