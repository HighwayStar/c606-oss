/*
 * Developer console: reads commands from stdin (USB-Serial-JTAG) so the UI
 * can be driven and screenshotted from the host without touching the
 * device. See devcon.h for the commands.
 *
 * The screen copy costs one memcpy per flushed area into PSRAM.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "board.h"
#include "devcon.h"
#include "ui_port.h"
#include "ride.h"

static const char *TAG = "devcon";

static uint16_t *s_frame;                 /* LCD_H_RES x LCD_V_RES RGB565, PSRAM */
static devcon_key_cb_t s_key_cb;

void devcon_mirror(int x1, int y1, int x2, int y2, const void *px)
{
    if (!s_frame) return;
    const uint16_t *src = px;
    int w = x2 - x1;
    for (int y = y1; y < y2; y++, src += w) {
        memcpy(&s_frame[y * LCD_H_RES + x1], src, w * 2);
    }
}

static void write_all(const char *s, size_t n)
{
    while (n) {
        int w = usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(2000));
        if (w <= 0) return;   /* host went away */
        s += w;
        n -= w;
    }
}

static void cmd_shot(void)
{
    static const char hex[] = "0123456789abcdef";
    static char line[LCD_H_RES * 4 + 3];
    char hdr[32];
    int n = snprintf(hdr, sizeof hdr, "SHOT %d %d\n", LCD_H_RES, LCD_V_RES);
    write_all(hdr, n);
    for (int y = 0; y < LCD_V_RES; y++) {
        const uint8_t *row = (const uint8_t *)&s_frame[y * LCD_H_RES];
        char *p = line;
        *p++ = 'R';
        for (int i = 0; i < LCD_H_RES * 2; i++) {
            *p++ = hex[row[i] >> 4];
            *p++ = hex[row[i] & 15];
        }
        *p++ = '\n';
        write_all(line, p - line);
    }
    write_all("SHOT_END\n", 9);
}

static void handle(char *cmd)
{
    char *save;
    char *w = strtok_r(cmd, " \t", &save);
    if (!w) return;
    if (!strcmp(w, "key")) {
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save);
        if (a && b && s_key_cb) s_key_cb(atoi(a), atoi(b));
        printf("ok\n");
    } else if (!strcmp(w, "tap")) {
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save);
        if (a && b) ui_port_inject_touch(atoi(a), atoi(b), 120);
        printf("ok\n");
    } else if (!strcmp(w, "spd")) {   /* feed a speed sample to the auto pause logic */
        char *a = strtok_r(NULL, " ", &save);
        if (a) ride_speed(atof(a), true);
        printf("ok\n");
    } else if (!strcmp(w, "shot")) {
        cmd_shot();
    } else if (!strcmp(w, "heap")) {
        printf("heap int %u psram %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        printf("? %s\n", w);
    }
}

static void devcon_task(void *arg)
{
    char line[64];
    for (;;) {
        if (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\r\n")] = 0;
            handle(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

esp_err_t devcon_init(devcon_key_cb_t key_cb)
{
    s_key_cb = key_cb;
    s_frame = heap_caps_calloc(LCD_H_RES * LCD_V_RES, 2, MALLOC_CAP_SPIRAM);
    if (!s_frame) ESP_LOGW(TAG, "no PSRAM for the screen copy; 'shot' disabled");

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;   /* one hex row per write */
    cfg.rx_buffer_size = 256;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb_serial_jtag driver: %s", esp_err_to_name(err));
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    xTaskCreate(devcon_task, "devcon", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "ready: key/tap/shot/heap");
    return ESP_OK;
}
