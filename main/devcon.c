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
#include <dirent.h>
#include <sys/stat.h>
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
#include "mapview.h"
#include "gps.h"
#include "sun.h"
#include "route.h"
#include "nrf_link.h"
#include "shifting.h"

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

/* directory listing: one "name size" line per entry, then LS_END */
static void cmd_ls(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        printf("LS_END cannot open %s\n", path);
        return;
    }
    struct dirent *e;
    char full[300];
    struct stat st;
    while ((e = readdir(d))) {
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        long size = stat(full, &st) == 0 && !S_ISDIR(st.st_mode) ? (long)st.st_size : -1;
        printf("%s %ld\n", e->d_name, size);
    }
    closedir(d);
    printf("LS_END\n");
}

/* file transfer: "FILE <size>", hex rows prefixed with 'F', FILE_END */
static void cmd_get(const char *path)
{
    static const char hex[] = "0123456789abcdef";
    static uint8_t buf[512];
    static char line[sizeof buf * 2 + 3];
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FILE_END cannot open %s\n", path);
        return;
    }
    struct stat st;
    stat(path, &st);
    char hdr[32];
    int n = snprintf(hdr, sizeof hdr, "FILE %ld\n", (long)st.st_size);
    write_all(hdr, n);
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        char *p = line;
        *p++ = 'F';
        for (size_t i = 0; i < r; i++) {
            *p++ = hex[buf[i] >> 4];
            *p++ = hex[buf[i] & 15];
        }
        *p++ = '\n';
        write_all(line, p - line);
    }
    fclose(f);
    write_all("FILE_END\n", 9);
}

/* host -> device: "put <path> <size>", then hex lines prefixed with 'F'
 * until PUT_END; the device answers "PUT_OK <bytes>" or "PUT_ERR ..." */
static void cmd_put(const char *path, long size)
{
    char *dir = strdup(path);
    char *slash = dir ? strrchr(dir, '/') : NULL;
    if (slash && slash != dir) { *slash = 0; mkdir(dir, 0775); }
    free(dir);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("PUT_ERR cannot create %s\n", path); return; }
    printf("PUT_GO\n");
    static char line[1100];
    long got = 0;
    bool ok = true;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "PUT_END")) break;
        if (line[0] != 'F') continue;
        for (const char *h = line + 1; h[0] && h[1]; h += 2) {
            char b[3] = { h[0], h[1], 0 };
            uint8_t v = (uint8_t)strtoul(b, NULL, 16);
            if (fwrite(&v, 1, 1, f) != 1) ok = false;
            got++;
        }
        printf("PUT_ACK\n");   /* the host sends the next line only now (512-byte RX ring) */
    }
    fclose(f);
    if (ok && got == size) printf("PUT_OK %ld\n", got);
    else printf("PUT_ERR got %ld of %ld\n", got, size);
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
    } else if (!strcmp(w, "nrf")) {   /* raw frame to the nRF: nrf <type> <cmd> <payload hex bytes...> */
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save), *h;
        uint8_t pl[32];
        int n = 0;
        while (n < (int)sizeof pl && (h = strtok_r(NULL, " ", &save))) pl[n++] = strtol(h, NULL, 16);
        if (a && b) printf(nrf_link_send(strtol(a, NULL, 0), strtol(b, NULL, 16), pl, n) == ESP_OK ? "ok\n" : "nrf failed\n");
        else printf("nrf failed\n");
    } else if (!strcmp(w, "shift")) { /* synthetic ANT+ shifting page: shift <front> <rear> [ftot] [rtot] */
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save);
        char *c = strtok_r(NULL, " ", &save), *d = strtok_r(NULL, " ", &save);
        if (a && !strcmp(a, "off")) {
            shifting_reset();
        } else if (a && b) {
            int front = atoi(a), rear = atoi(b);
            int ftot = c ? atoi(c) : front, rtot = d ? atoi(d) : rear;
            uint8_t pg[8] = { 0x01, 0, 0xFF, 0, 0, 0xFF, 0xFF, 0xFF };
            pg[3] = ((front > 0 ? (front - 1) & 7 : 0x07) << 5) | ((rear > 0 ? rear - 1 : 0x1F) & 0x1F);
            pg[4] = (((front > 0 ? ftot : 0) & 7) << 5) | ((rear > 0 ? rtot : 0) & 0x1F);
            shifting_page(pg);
        }
        printf("ok\n");
    } else if (!strcmp(w, "di2")) {   /* synthetic Di2 page: di2 <front> <rear> [batt%] | di2 speeds <f> <r> */
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save);
        char *c = strtok_r(NULL, " ", &save);
        uint8_t pg[8] = { 0 };
        if (a && b && !strcmp(a, "speeds")) {
            pg[0] = 0x11;
            pg[2] = atoi(b);
            pg[3] = c ? atoi(c) : 0;
            shifting_di2_page(pg);
        } else if (a && b) {
            pg[2] = atoi(a);
            pg[3] = atoi(b);
            pg[4] = c ? atoi(c) : 0;
            shifting_di2_page(pg);
        }
        printf("ok\n");
    } else if (!strcmp(w, "shot")) {
        cmd_shot();
    } else if (!strcmp(w, "ls")) {
        char *a = strtok_r(NULL, " ", &save);
        cmd_ls(a ? a : "/sdcard/c606oss");
    } else if (!strcmp(w, "get")) {
        char *a = strtok_r(NULL, " ", &save);
        if (a) cmd_get(a);
    } else if (!strcmp(w, "mv")) {    /* rename a file on the card */
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save);
        if (a && b) printf(rename(a, b) == 0 ? "ok\n" : "mv failed\n");
        else printf("mv failed\n");
    } else if (!strcmp(w, "nmea")) {  /* simulated GPS: one sentence, or "nmea off" */
        char *a = strtok_r(NULL, " ", &save);
        if (a && !strcmp(a, "off")) { gps_simulate(false); printf("ok\n"); }
        else printf(a && gps_inject(a) ? "ok\n" : "nmea failed\n");
    } else if (!strcmp(w, "pos")) {   /* centre the map page on a fixed position */
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save);
        if (a && b) {
            mapview_set_override(atof(a), atof(b), true);
            sun_set_position(atof(a), atof(b));
            route_track(atof(a), atof(b));
        }
        else mapview_set_override(0, 0, false);
        printf("ok\n");
    } else if (!strcmp(w, "zoom")) {
        char *a = strtok_r(NULL, " ", &save);
        if (a) mapview_zoom_by(atoi(a) - mapview_zoom());
        printf("zoom %u\n", mapview_zoom());
    } else if (!strcmp(w, "put")) {
        char *a = strtok_r(NULL, " ", &save), *b = strtok_r(NULL, " ", &save);
        if (a && b) cmd_put(a, atol(b));
        else printf("PUT_ERR usage\n");
    } else if (!strcmp(w, "heap")) {
        printf("heap int %u psram %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        printf("? %s\n", w);
    }
}

static void devcon_task(void *arg)
{
    char line[320];
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
    cfg.rx_buffer_size = 512;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb_serial_jtag driver: %s", esp_err_to_name(err));
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    xTaskCreate(devcon_task, "devcon", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "ready: key/tap/spd/shot/ls/get/put/mv/pos/zoom/nmea/nrf/heap");
    return ESP_OK;
}
