/*
 * Magene C606 open firmware - proof of concept.
 *
 * Brings up the ST7789 over the i80 bus, LVGL 9 with draw buffers in internal
 * RAM and objects in PSRAM, the backlight, and the UART link to the nRF
 * co-processor, and the GNSS receiver on UART0.
 * Keys: 0 = switch page, 1/2 = backlight down/up, hold 2 = start/stop
 * recording a CSV track, hold 1 = USB mass storage mode (hold again: reboot),
 * hold 0 = power off (nRF cuts power; next power-on is a full POR).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "lcd.h"
#include "backlight.h"
#include "nrf_link.h"
#include "ui_port.h"
#include "ui.h"
#include "gps.h"
#include "sdcard.h"
#include "tracklog.h"
#include "usb_msc.h"
#include "esp_system.h"
#include "tinyusb.h"

static const char *TAG = "main";

static bool s_nrf_alive;
static uint8_t s_nrf_reason;
static uint8_t s_nrf_fw[3];
static uint8_t s_bl_pct = 70;
static int16_t s_temp_c100;
static uint32_t s_press_pa100;

static void set_backlight(int pct)
{
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    s_bl_pct = pct;
    backlight_set(pct);
    ui_set_backlight(pct);
}

static void on_key(const nrf_key_event_t *ev)
{
    ESP_LOGI(TAG, "KEY idx=%u event=%u aux=%u", ev->key, ev->event, ev->aux);
    ui_key_event(ev->key, ev->event);
    if (ev->key == 1 && ev->event == KEY_EVT_LONG_RELEASE) {
        if (usb_msc_active()) {
            usb_msc_leave_and_restart();   /* restores USB-Serial-JTAG, then reboots */
        }
        if (usb_msc_enter() == ESP_OK) {
            ui_show_usb_mode();
        } else {
            ESP_LOGW(TAG, "USB mode failed");
        }
        return;
    }
    if (ev->key == 0 && ev->event == KEY_EVT_LONG_RELEASE) {
        /* vendor behaviour: long press on the power key -> ask the nRF to cut
         * power. Coming back is a true power-on reset. */
        ESP_LOGI(TAG, "power off requested");
        tracklog_stop();
        if (usb_msc_active()) {
            tinyusb_driver_uninstall();
        }
        usb_phy_route_to_serial_jtag();
        nrf_link_send_power_off();
        return;
    }
    if (usb_msc_active()) {
        return;   /* only the holds above are meaningful in USB mode */
    }
    if (ev->key == 2 && ev->event == KEY_EVT_LONG_RELEASE) {
        if (tracklog_active()) {
            tracklog_stop();
        } else {
            gps_fix_t fix;
            gps_get(&fix);
            if (tracklog_start(&fix) != ESP_OK) {
                ESP_LOGW(TAG, "cannot start recording (SD?)");
            }
        }
        ui_set_rec(tracklog_active(), tracklog_points());
        return;
    }
    if (ev->event != KEY_EVT_CLICK) {
        return;
    }
    switch (ev->key) {
    case 0: ui_toggle_page(); break;
    case 1: set_backlight(s_bl_pct - 10); break;
    case 2: set_backlight(s_bl_pct + 10); break;
    default: break;
    }
}

/* Runs in the nRF link task. */
static void on_frame(const uint8_t *f, size_t len, void *ctx)
{
    const uint8_t *p = f + 6;
    uint8_t cmd = f[5];
    nrf_key_event_t ev;

    if (nrf_link_decode_key(f, len, &ev)) {
        on_key(&ev);
    } else if (cmd == NRF_CMD_SYS && p[0] == NRF_SYS_CTRL && p[1] == 0x02) {
        if (p[2] == 0x00) s_nrf_reason = p[5];   /* power-on reason */
        s_nrf_alive = true;
        ui_set_nrf(true, s_nrf_reason, s_nrf_fw);
    } else if (cmd == 0x01 && p[0] == 0x01 && len >= 18) {
        s_nrf_fw[0] = p[7]; s_nrf_fw[1] = p[8]; s_nrf_fw[2] = p[9];
        ui_set_nrf(s_nrf_alive, s_nrf_reason, s_nrf_fw);
    } else if (cmd == 0x00 && p[0] == 0x52) {
        ui_set_battery(p[6], p[4] | (p[5] << 8));
    } else if (cmd == NRF_CMD_SYS && p[0] == 0xF1 && p[1] == 0x03) {
        s_temp_c100 = p[2] | (p[3] << 8);
        s_press_pa100 = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
        ui_set_env(s_temp_c100, s_press_pa100);
    } else if (!(cmd == 0x00 && p[0] == 0x53) && !(cmd == NRF_CMD_SYS && (p[0] == 0xF0 || p[0] == 0xF1))) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, f, len, ESP_LOG_INFO); /* anything not yet understood */
    }
}

static void on_gps(const gps_fix_t *fix, void *ctx)
{
    static uint32_t last_log;
    ui_set_gps(fix);
    if (tracklog_active()) {
        tracklog_point(fix, s_temp_c100, s_press_pa100);
        ui_set_rec(true, tracklog_points());
    }
    if (fix->last_rx_ms - last_log > 5000) {
        last_log = fix->last_rx_ms;
        ESP_LOGI(TAG, "GPS %s q=%u sats %u/%u hdop %.1f  %.5f %.5f alt %.0f  %.1f km/h  %02u:%02u:%02u  n=%lu bad=%lu",
                 fix->valid ? "FIX" : "nofix", fix->fix_quality, fix->sats_used, fix->sats_in_view, fix->hdop,
                 fix->lat, fix->lon, fix->alt_m, fix->speed_kmh, fix->hh, fix->mm, fix->ss,
                 (unsigned long)fix->sentences, (unsigned long)fix->bad_checksum);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "heap: internal %u KB, psram %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* if the previous run ended in USB-MSC mode the PHY mux still points at
     * the OTG controller (RTC register): put it back so the console works */
    usb_phy_route_to_serial_jtag();

    ESP_ERROR_CHECK(backlight_init());
    ESP_ERROR_CHECK(lcd_init());
    ESP_ERROR_CHECK(ui_port_init());
    ui_create();
    vTaskDelay(pdMS_TO_TICKS(100));      /* let the first frame render */
    set_backlight(s_bl_pct);

    ESP_ERROR_CHECK(nrf_link_init(on_frame, NULL));
    ESP_ERROR_CHECK(gps_init(on_gps, NULL));

    if (sdcard_mount() == ESP_OK) {
        const sdcard_info_t *sd = sdcard_info();
        ui_set_sd(true, sd->name, sd->size_mb);
    }

    uint32_t last_pwr_ms = 0;
    bool first = true;
    for (;;) {
        uint32_t now = esp_timer_get_time() / 1000;
        /* vendor INIT_QUERY: SendPowerOnCmd every 1 s until acked, then keep-alive */
        uint32_t period = s_nrf_alive ? 5000 : 1000;
        if (first || now - last_pwr_ms >= period) {
            first = false;
            last_pwr_ms = now;
            nrf_link_send_power_on();
            if (!s_nrf_alive) {
                nrf_link_send_gps_power(NRF_GPS_ON);  /* vendor default; harmless if already on */
            }
        }
        ui_tick(now / 1000);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
