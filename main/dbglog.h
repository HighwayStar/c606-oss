#pragma once
#include <stdbool.h>

/* Debug log on the eMMC (Settings -> System -> Debug log), for testing on
 * devices without a USB console attached. Off by default.
 *
 * Every ESP_LOGx line (what the USB console shows, colour codes stripped)
 * goes into a PSRAM ring buffer; a low-priority task appends it to
 * /sdcard/c606oss/logs/logNNNN.txt about once a second (open, append,
 * close: the file stays readable and committed), so a
 * crash or power loss costs at most the last second. A new file per boot
 * (and every DBGLOG_FILE_MAX bytes), the newest DBGLOG_KEEP files are kept.
 * Each file starts with firmware version, board and reset reason; a UTC
 * marker line once a minute ties the uptime stamps to the wall clock.
 * Lines logged before the card is mounted are kept in the ring. */

#define DBGLOG_DIR      "/sdcard/c606oss/logs"
#define DBGLOG_FILE_MAX (4 * 1024 * 1024)
#define DBGLOG_KEEP     10

/* After config_load(): installs the log hook, starts logging when `enabled`. */
void dbglog_init(bool enabled);
void dbglog_set_enabled(bool on);
bool dbglog_enabled(void);
/* Writes what is buffered and closes the file for good (USB storage mode,
 * power off): the card is about to go away. */
void dbglog_shutdown(void);
