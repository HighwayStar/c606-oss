/*
 * GNSS receiver (Airoha AG3352 on the C606 and C706) on UART0, TX GPIO1 / RX GPIO0.
 *
 * Vendor: MidCommInit(0, 921600, ...); GpsPthreadMachine() sends a handful
 * of $PAIR commands (867/866/490/491/470) that only tune EPO/AGNSS
 * behaviour - plain NMEA is on by default, so we just listen. The chip is
 * powered by the nRF (E2 02 07: 0 off, 1 on, 2 hard reset).
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"

#include "board.h"
#include "gps.h"

static const char *TAG = "gps";

static gps_fix_t s_fix;
static SemaphoreHandle_t s_lock;
static gps_update_cb_t s_cb;
static void *s_cb_ctx;
static volatile bool s_simulated;   /* devcon feeds sentences, UART ignored */

/* the board's vendor baud first, then the usual GNSS defaults (C606 opens
 * the module at 921600, C706 at 115200; a repeat in the list is harmless) */
static const uint32_t k_bauds[] = {GPS_UART_BAUD, 921600, 115200, 9600, 460800, 230400, 38400, 57600};

static bool nmea_checksum_ok(const char *s, size_t n)
{
    /* s = "$....*hh" without CRLF */
    if (n < 4 || s[0] != '$' || s[n - 3] != '*') return false;
    uint8_t x = 0;
    for (size_t i = 1; i < n - 3; i++) x ^= (uint8_t)s[i];
    return strtoul(s + n - 2, NULL, 16) == x;
}

/* ddmm.mmmm[m] -> degrees */
static double nmea_coord(const char *f, char hemi)
{
    if (!f || !*f) return 0;
    double v = atof(f);
    int deg = (int)(v / 100);
    double d = deg + (v - deg * 100) / 60.0;
    return (hemi == 'S' || hemi == 'W') ? -d : d;
}

/* Split on ',' in place; returns field count. Empty fields become "". */
static int split(char *s, char *f[], int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++) {
        if (*s == ',' || *s == '*') {
            *s = 0;
            f[n++] = s + 1;
        }
    }
    return n;
}

static void parse(char *line, size_t n)
{
    char *f[24];
    int nf = split(line, f, 24);
    if (nf < 2 || strlen(f[0]) < 6) return;
    const char *type = f[0] + 3;   /* skip "$GN" / "$GP" / "$BD" ... */
    bool changed = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fix.sentences++;
    s_fix.last_rx_ms = esp_timer_get_time() / 1000;

    if (!strcmp(type, "RMC") && nf >= 10) {
        /* 1 time, 2 status, 3 lat, 4 N/S, 5 lon, 6 E/W, 7 sog kn, 8 cog, 9 date */
        if (strlen(f[1]) >= 6) {
            s_fix.hh = (f[1][0] - '0') * 10 + f[1][1] - '0';
            s_fix.mm = (f[1][2] - '0') * 10 + f[1][3] - '0';
            s_fix.ss = (f[1][4] - '0') * 10 + f[1][5] - '0';
        }
        s_fix.valid = f[2][0] == 'A';
        if (s_fix.valid) {
            s_fix.lat = nmea_coord(f[3], f[4][0]);
            s_fix.lon = nmea_coord(f[5], f[6][0]);
            s_fix.speed_kmh = atof(f[7]) * 1.852f;
            s_fix.course_deg = atof(f[8]);
        }
        if (strlen(f[9]) >= 6) {
            s_fix.day = (f[9][0] - '0') * 10 + f[9][1] - '0';
            s_fix.mon = (f[9][2] - '0') * 10 + f[9][3] - '0';
            s_fix.year = (f[9][4] - '0') * 10 + f[9][5] - '0';
        }
        changed = true;
    } else if (!strcmp(type, "GGA") && nf >= 10) {
        /* 6 quality, 7 sats used, 8 hdop, 9 altitude */
        s_fix.fix_quality = atoi(f[6]);
        s_fix.sats_used = atoi(f[7]);
        s_fix.hdop = atof(f[8]);
        if (f[9][0]) s_fix.alt_m = atof(f[9]);
        changed = true;
    } else if (!strcmp(type, "GSV") && nf >= 4) {
        /* 1 total msgs, 2 msg no, 3 sats in view (per talker) */
        static uint8_t per_talker[8];
        static char talkers[8][3];
        int idx = -1;
        for (int i = 0; i < 8; i++) {
            if (!talkers[i][0]) { strncpy(talkers[i], f[0] + 1, 2); idx = i; break; }
            if (!strncmp(talkers[i], f[0] + 1, 2)) { idx = i; break; }
        }
        if (idx >= 0) per_talker[idx] = atoi(f[3]);
        int sum = 0;
        for (int i = 0; i < 8; i++) sum += per_talker[i];
        s_fix.sats_in_view = sum > 255 ? 255 : sum;
    }
    gps_fix_t snap = s_fix;
    xSemaphoreGive(s_lock);

    if (changed && s_cb) {
        s_cb(&snap, s_cb_ctx);
    }
}

static void set_baud(uint32_t baud)
{
    uart_set_baudrate(GPS_UART_NUM, baud);
    uart_flush_input(GPS_UART_NUM);
}

static void gps_task(void *arg)
{
    static char line[256];
    size_t fill = 0;
    int baud_idx = 0;
    uint32_t probe_start = esp_timer_get_time() / 1000;
    uint32_t raw_logged = 0;

    set_baud(k_bauds[0]);
    for (;;) {
        uint8_t buf[256];
        int n = uart_read_bytes(GPS_UART_NUM, buf, sizeof buf, pdMS_TO_TICKS(100));
        uint32_t now = esp_timer_get_time() / 1000;

        /* baud probing: rotate every 2 s until a sentence with a valid checksum arrives */
        if (s_fix.baud == 0 && now - probe_start > 2000) {
            baud_idx = (baud_idx + 1) % (sizeof k_bauds / sizeof k_bauds[0]);
            probe_start = now;
            fill = 0;
            set_baud(k_bauds[baud_idx]);
            ESP_LOGI(TAG, "probing %lu baud", (unsigned long)k_bauds[baud_idx]);
            continue;
        }

        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '$') {
                fill = 0;
            }
            if (c == '\n' || c == '\r') {
                if (fill > 0) {
                    line[fill] = 0;
                    if (line[0] == '$' && nmea_checksum_ok(line, fill)) {
                        if (s_fix.baud == 0) {
                            s_fix.baud = k_bauds[baud_idx];
                            ESP_LOGI(TAG, "NMEA detected at %lu baud", (unsigned long)s_fix.baud);
                        }
                        if (raw_logged < 8) {
                            raw_logged++;
                            ESP_LOGI(TAG, "%s", line);
                        }
                        if (!s_simulated) parse(line, fill);
                    } else if (line[0] == '$' && s_fix.baud) {
                        s_fix.bad_checksum++;
                    }
                }
                fill = 0;
            } else if (fill < sizeof line - 1) {
                line[fill++] = c;
            } else {
                fill = 0; /* overlong garbage */
            }
        }
    }
}

esp_err_t gps_init(gps_update_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    uart_config_t cfg = {
        .baud_rate = GPS_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* vendor: rx buf 0x1000, tx buf 0x400 */
    ESP_RETURN_ON_ERROR(uart_driver_install(GPS_UART_NUM, 4096, 1024, 0, NULL, 0), TAG, "install");
    ESP_RETURN_ON_ERROR(uart_param_config(GPS_UART_NUM, &cfg), TAG, "param");
    ESP_RETURN_ON_ERROR(uart_set_pin(GPS_UART_NUM, GPS_UART_TX, GPS_UART_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pins");

    xTaskCreate(gps_task, "gps", 6144, NULL, 9, NULL);
    ESP_LOGI(TAG, "UART%d tx=%d rx=%d, probing from %d baud", GPS_UART_NUM, GPS_UART_TX, GPS_UART_RX, GPS_UART_BAUD);
    return ESP_OK;
}

bool gps_inject(const char *sentence)
{
    char line[128];
    size_t n = strlen(sentence);
    if (n < 8 || n >= sizeof line || sentence[0] != '$' || !nmea_checksum_ok(sentence, n)) return false;
    memcpy(line, sentence, n + 1);
    s_simulated = true;
    if (s_fix.baud == 0) s_fix.baud = 1;   /* "data seen" for the UI */
    parse(line, n);
    return true;
}

void gps_simulate(bool on) { s_simulated = on; }

void gps_get(gps_fix_t *out)
{
    if (!s_lock) {            /* before gps_init(): nothing received yet */
        memset(out, 0, sizeof *out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_fix;
    xSemaphoreGive(s_lock);
}
