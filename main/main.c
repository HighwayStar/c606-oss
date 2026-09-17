/*
 * Magene C606 open firmware - proof of concept.
 *
 * Brings up the ST7789 over the i80 bus, turns on the backlight, opens the
 * UART link to the nRF co-processor, performs the vendor's power-on
 * handshake and shows every button event (and every raw frame) on screen.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"

#include "board.h"
#include "lcd.h"
#include "backlight.h"
#include "nrf_link.h"

static const char *TAG = "main";

#define KEY_LOG_LEN 5
#define KEY_MAX     5

typedef struct {
    nrf_key_event_t ev;
    uint32_t t_ms;
} key_log_entry_t;

static struct {
    SemaphoreHandle_t lock;
    uint32_t rx_frames;
    uint32_t rx_keys;
    bool nrf_alive;            /* saw any reply to our power-on command */
    uint8_t bat_pct;           /* cmd 0 'R': payload[6] */
    uint16_t bat_mv;           /* cmd 0 'R': payload[4..5] */
    uint8_t status;            /* cmd 0x10 F0 02: payload[2] */
    uint8_t pwr_reason;        /* cmd 0x10 E2 02 00: payload[5] */
    uint8_t fw[3];             /* cmd 1 sub 1: payload[7..9] */
    uint8_t last_frame[NRF_MAX_FRAME];
    size_t last_frame_len;
    key_log_entry_t keys[KEY_LOG_LEN];
    uint8_t key_state[KEY_MAX]; /* last event value per key idx */
    bool dirty;
} s;

static void on_frame(const uint8_t *f, size_t len, void *ctx)
{
    nrf_key_event_t ev;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.rx_frames++;
    /* periodic battery/status frames are noisy; log the rest */
    if (!(f[5] == 0x00 && f[6] == 0x52) && !(f[5] == NRF_CMD_SYS && f[6] == 0xF0)) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, f, len, ESP_LOG_INFO);
    }
    s.last_frame_len = len < sizeof s.last_frame ? len : sizeof s.last_frame;
    memcpy(s.last_frame, f, s.last_frame_len);

    /* Any E2-02 reply means the nRF acknowledged our power-on request. */
    const uint8_t *p = f + 6;
    if (f[5] == NRF_CMD_SYS && p[0] == NRF_SYS_CTRL && p[1] == 0x02) {
        s.nrf_alive = true;
        if (p[2] == 0x00) {
            s.pwr_reason = p[5];
        }
    } else if (f[5] == NRF_CMD_SYS && p[0] == 0xF0 && p[1] == 0x02) {
        s.status = p[2];
    } else if (f[5] == 0x00 && p[0] == 0x52) {
        s.bat_mv = p[4] | (p[5] << 8);
        s.bat_pct = p[6];
    } else if (f[5] == 0x01 && p[0] == 0x01 && len >= 18) {
        s.fw[0] = p[7]; s.fw[1] = p[8]; s.fw[2] = p[9];
    }
    if (nrf_link_decode_key(f, len, &ev)) {
        s.rx_keys++;
        memmove(&s.keys[1], &s.keys[0], sizeof(key_log_entry_t) * (KEY_LOG_LEN - 1));
        s.keys[0].ev = ev;
        s.keys[0].t_ms = esp_timer_get_time() / 1000;
        if (ev.key < KEY_MAX) {
            s.key_state[ev.key] = ev.event;
        }
        ESP_LOGI(TAG, "KEY idx=%u event=%u aux=%u", ev.key, ev.event, ev.aux);
    }
    s.dirty = true;
    xSemaphoreGive(s.lock);
}

static void draw_screen(void)
{
    char line[32];
    const int cols = lcd_text_cols();   /* 20 */
    const uint32_t up_s = esp_timer_get_time() / 1000000;

    lcd_fill(C_BLACK);

    /* title bar */
    lcd_fill_rect(0, 0, LCD_H_RES, 20, C_BLUE);
    lcd_text_rc(0, 0, " C606 open FW  PoC  ", C_WHITE, C_BLUE);

    snprintf(line, sizeof line, "up %lus  rx %lu fr", (unsigned long)up_s, (unsigned long)s.rx_frames);
    lcd_text_rc(0, 1, line, C_WHITE, C_BLACK);

    if (s.nrf_alive) {
        snprintf(line, sizeof line, "nRF ok r%u fw%u.%u.%u", s.pwr_reason, s.fw[0], s.fw[1], s.fw[2]);
    } else {
        snprintf(line, sizeof line, "nRF: no reply yet");
    }
    lcd_text_rc(0, 2, line, s.nrf_alive ? C_GREEN : C_ORANGE, C_BLACK);
    snprintf(line, sizeof line, "bat %u%% %umV st%u", s.bat_pct, s.bat_mv, s.status);
    lcd_text_rc(0, 3, line, C_WHITE, C_BLACK);

    /* key state boxes: one per key index, lit while the last event != 0 */
    lcd_text_rc(0, 4, "keys:", C_GREY, C_BLACK);
    for (int k = 0; k < KEY_MAX; k++) {
        int x = 66 + k * 34, y = 4 * 20;
        uint8_t st = s.key_state[k];
        uint16_t c = st == 0 ? C_DGREY : (st == KEY_EVT_LONG_START || st == KEY_EVT_HOLD_REPEAT) ? C_RED : C_GREEN;
        lcd_fill_rect(x, y, 30, 20, c);
        snprintf(line, sizeof line, "%d", k);
        lcd_draw_text(x + 9, y, line, C_WHITE, c);
    }

    /* recent key events */
    lcd_text_rc(0, 5, "time    key evt aux", C_YELLOW, C_BLACK);
    for (int i = 0; i < KEY_LOG_LEN; i++) {
        const key_log_entry_t *e = &s.keys[i];
        if (e->t_ms == 0) {
            break;
        }
        snprintf(line, sizeof line, "%6lu.%lu  %u   %u   %u",
                 (unsigned long)(e->t_ms / 1000), (unsigned long)((e->t_ms / 100) % 10),
                 e->ev.key, e->ev.event, e->ev.aux);
        lcd_text_rc(0, 6 + i, line, i == 0 ? C_WHITE : C_GREY, C_BLACK);
    }

    /* last raw frame as hex, 6 bytes per row */
    lcd_text_rc(0, 11, "last frame:", C_YELLOW, C_BLACK);
    for (size_t i = 0; i < s.last_frame_len && i < 18; i += 6) {
        int n = 0;
        for (size_t j = i; j < i + 6 && j < s.last_frame_len; j++) {
            n += snprintf(line + n, sizeof line - n, "%02x ", s.last_frame[j]);
        }
        lcd_text_rc(1, 12 + i / 6, line, C_CYAN, C_BLACK);
    }

    /* footer: build id */
    const esp_app_desc_t *ad = esp_app_get_description();
    snprintf(line, sizeof line, "%-*.*s", cols, cols, ad->version);
    lcd_fill_rect(0, LCD_V_RES - 20, LCD_H_RES, 20, C_DGREY);
    lcd_text_rc(0, 15, line, C_WHITE, C_DGREY);

    lcd_flush();
}

void app_main(void)
{
    s.lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(backlight_init());
    ESP_ERROR_CHECK(lcd_init());

    /* Something visible as early as possible: solid colour bars. */
    lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES / 3, C_RED);
    lcd_fill_rect(0, LCD_V_RES / 3, LCD_H_RES, LCD_V_RES / 3, C_GREEN);
    lcd_fill_rect(0, 2 * LCD_V_RES / 3, LCD_H_RES, LCD_V_RES / 3, C_BLUE);
    lcd_draw_text(24, LCD_V_RES / 2 - 10, "C606 open FW", C_WHITE, C_GREEN);
    lcd_flush();
    backlight_set(70);
    vTaskDelay(pdMS_TO_TICKS(800));

    ESP_ERROR_CHECK(nrf_link_init(on_frame, NULL));

    uint32_t last_pwr_ms = 0;
    bool first = true;
    for (;;) {
        uint32_t now = esp_timer_get_time() / 1000;

        /* Vendor INIT_QUERY state: resend SendPowerOnCmd every second until
         * the nRF answers. We keep resending every 5 s afterwards as a cheap
         * keep-alive in case the nRF expects periodic traffic. */
        uint32_t period = s.nrf_alive ? 5000 : 1000;
        if (first || now - last_pwr_ms >= period) {
            first = false;
            last_pwr_ms = now;
            nrf_link_send_power_on();
        }

        xSemaphoreTake(s.lock, portMAX_DELAY);
        draw_screen();
        s.dirty = false;
        xSemaphoreGive(s.lock);

        /* redraw on activity, or every 500 ms for the uptime counter */
        for (int i = 0; i < 25 && !s.dirty; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
