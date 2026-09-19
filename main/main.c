/*
 * Magene C606 / C706 open firmware - proof of concept.
 *
 * Brings up the LCD over the i80 bus, LVGL 9 with draw buffers in internal
 * RAM and objects in PSRAM, the backlight, and the UART link to the nRF
 * co-processor, and the GNSS receiver on UART0.
 * Keys (roles per board in board_*.h): KEY_NEXT_PAGE / KEY_PREV_PAGE,
 * KEY_LAP = manual lap, KEY_RIDE = start ride / pause / resume, hold
 * KEY_RIDE = end ride (dialog), hold KEY_POWER = power off (nRF cuts power;
 * next power-on is a full POR). USB mass storage mode: Settings -> System.
 * Rides are recorded as FIT files (tracklog.c) on the eMMC.
 */
#include <stdio.h>
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
#include "sun.h"
#include "sdcard.h"
#include "tracklog.h"
#include "usb_msc.h"
#include "mapview.h"
#include "route.h"
#include "touch.h"
#include "ant.h"
#include "sensor_list.h"
#include "stats.h"
#include "fields.h"
#include "config.h"
#include "devcon.h"
#include "ride.h"
#include "trip.h"
#include "utc.h"
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

static void on_ride_mode(ride_mode_t mode)
{
    ui_set_mode(mode);
}

/* "End ride?" confirmed: close the ride and show what it was */
static void end_ride(void)
{
    ride_end();
    ui_show_summary();
}

static void apply_backlight(void)
{
    set_backlight(config_get()->backlight);
}

/* KEY_LAP during a ride: manual lap */
static void manual_lap(void)
{
    char msg[16];
    if (ride_mode() == RIDE_IDLE) return;
    uint32_t n = trip_lap_manual();
    snprintf(msg, sizeof msg, "LAP %lu", (unsigned long)n);
    ui_toast(msg);
}

static void enter_usb_mode(void)
{
    if (usb_msc_active()) {
        return;
    }
    if (usb_msc_enter() == ESP_OK) {
        ui_show_usb_mode();
    } else {
        ESP_LOGW(TAG, "USB mode failed");
    }
}

static void power_off(void)
{
    ESP_LOGI(TAG, "power off");
    ride_end();
    if (usb_msc_active()) {
        tinyusb_driver_uninstall();
    }
    usb_phy_route_to_serial_jtag();
    nrf_link_send_power_off();
}

static void on_key(const nrf_key_event_t *ev)
{
    ESP_LOGI(TAG, "KEY idx=%u event=%u aux=%u", ev->key, ev->event, ev->aux);
    ui_key_event(ev->key, ev->event);
    if (ev->key == KEY_USB_EXIT && ev->event == KEY_EVT_LONG_RELEASE && usb_msc_active()) {
        usb_msc_leave_and_restart();   /* restores USB-Serial-JTAG, then reboots */
        return;
    }
    if (ev->key == KEY_POWER && ev->event == KEY_EVT_LONG_RELEASE) {
        /* vendor behaviour: long press on the power key -> confirmation popup.
         * Holding the key also makes the nRF start its IMU/baro/RTC stream
         * (it does that as part of its own power-on gesture), so after a
         * USB/software reset one hold + cancel re-enables temperature. */
        ui_show_power_popup();
        return;
    }
    ui_popup_t popup = ui_popup_active();
    if (popup != UI_POPUP_NONE) {
        /* the key that opened the popup confirms, anything else cancels */
        if (ev->event == KEY_EVT_CLICK) {
            ui_hide_popup();
            if (popup == UI_POPUP_POWER && ev->key == KEY_POWER) power_off();
            if (popup == UI_POPUP_END_RIDE && ev->key == KEY_RIDE) end_ride();
        }
        return;
    }
    if (usb_msc_active()) {
        return;   /* only the holds above are meaningful in USB mode */
    }
    if (ui_summary_active()) {
        if (ev->event == KEY_EVT_CLICK) ui_hide_summary();
        return;
    }
    if (ui_menu_active() && ui_menu_key(ev->key, ev->event)) {
        return;   /* settings menu: KEY_MENU_UP / DOWN / SELECT / BACK */
    }
    if (ev->key == KEY_RIDE && ev->event == KEY_EVT_LONG_RELEASE) {
        if (ride_mode() != RIDE_IDLE) ui_show_end_ride_popup();
        return;
    }
    if (ev->event != KEY_EVT_CLICK) {
        return;
    }
    /* roles may share a key (C706: lap = click on the power key), so no switch */
    if (ev->key == KEY_RIDE) ride_toggle();        /* start / pause / resume */
    if (ev->key == KEY_LAP) manual_lap();
    if (ev->key == KEY_NEXT_PAGE) ui_next_page();
    if (ev->key == KEY_PREV_PAGE) ui_prev_page();
}

static void inject_key(uint8_t key, uint8_t evt)
{
    nrf_key_event_t ev = { .key = key, .event = evt };
    on_key(&ev);
}

static void on_ant(const ant_sensors_t *v, void *ctx)
{
    static uint32_t last_log;
    size_t n;
    const ant_channel_t *ch = ant_channels(&n);
    ui_set_sensors(v, ch, n);
    if (ant_live(ANT_DEV_HR, ANT_DEV_HR) && v->hr_bpm) stats_update(STAT_HR, v->hr_bpm);
    if (ant_live(ANT_DEV_CADENCE, ANT_DEV_SPD_CAD)) stats_update(STAT_CADENCE, v->cadence_rpm);
    if (ant_live(ANT_DEV_SPEED, ANT_DEV_SPD_CAD)) stats_update(STAT_ANT_SPEED, v->speed_kmh);
    if (ant_live(ANT_DEV_POWER, ANT_DEV_POWER)) stats_update(STAT_POWER, v->power_w);
    trip_wheel(v->wheel_revs, ant_live(ANT_DEV_SPEED, ANT_DEV_SPD_CAD));
    uint32_t now = esp_timer_get_time() / 1000;
    if (now - last_log >= 1000) {
        last_log = now;
        ESP_LOGI(TAG, "SENSORS hr=%u cad=%.1f spd=%.2f km/h revs=%lu pwr=%u",
                 v->hr_bpm, v->cadence_rpm, v->speed_kmh, (unsigned long)v->wheel_revs, v->power_w);
    }
}

/* Called ~8 s after boot. A channel the nRF already reports as connected is
 * only tracked; everything else gets a connect command. */
static void connect_paired_sensors(void)
{
    app_cfg_t *cfg = config_get();
    if (!cfg->sensors_imported) {
        /* first run: take over the sensors paired with the vendor firmware */
        static sensor_entry_t list[CFG_MAX_SENSORS];
        size_t n = sensor_list_load(list, CFG_MAX_SENSORS);
        for (size_t i = 0; i < n; i++) {
            config_sensor_add(list[i].ant_dev_type, list[i].dev_num, list[i].trans_type);
        }
        cfg->sensors_imported = 1;
        config_save();
        ESP_LOGI(TAG, "imported %u sensors from the vendor list", (unsigned)n);
    }
    for (int i = 0; i < cfg->nsensors; i++) {
        const cfg_sensor_t *e = &cfg->sensors[i];
        if (ant_nrf_live(e->dev_type)) {
            ant_track(e->dev_type, e->dev_num, e->trans_type);
        } else {
            ant_connect(e->dev_type, e->dev_num, e->trans_type);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    size_t nch;
    const ant_channel_t *ch = ant_channels(&nch);
    ant_sensors_t v;
    ant_get(&v);
    ui_set_sensors(&v, ch, nch);
}

/* Runs in the nRF link task. */
static void on_frame(const uint8_t *f, size_t len, void *ctx)
{
    const uint8_t *p = f + 6;
    uint8_t cmd = f[5];
    nrf_key_event_t ev;

    if (ant_handle_frame(f, len)) {
        return;
    }
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
        stats_update(STAT_BATTERY, p[4] | (p[5] << 8));
        fields_set_battery_pct(p[6]);
    } else if (cmd == 0x00 && p[0] == 0x53) {
        utc_set_rtc(p[2], p[3], p[4], p[5], p[6], p[7]);   /* nRF RTC: ss mm hh dd MM yy, UTC */
    } else if (cmd == NRF_CMD_SYS && p[0] == 0xF1 && p[1] == 0x03) {
        s_temp_c100 = p[2] | (p[3] << 8);
        s_press_pa100 = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
        ui_set_env(s_temp_c100, s_press_pa100);
        stats_update(STAT_TEMP, s_temp_c100 / 100.0f);
        stats_update(STAT_PRESSURE, s_press_pa100 / 10000.0f);
    } else if (!(cmd == NRF_CMD_SYS && (p[0] == 0xF0 || p[0] == 0xF1))) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, f, len, ESP_LOG_INFO); /* anything not yet understood */
    }
}

static void on_gps(const gps_fix_t *fix, void *ctx)
{
    static uint32_t last_log;
    ui_set_gps(fix);
    if (fix->valid) {
        stats_update(STAT_SPEED, fix->speed_kmh);
        stats_update(STAT_ALTITUDE, fix->alt_m);
        sun_set_position(fix->lat, fix->lon);
        route_track(fix->lat, fix->lon);
    }
    trip_gps(fix);
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
    touch_init();                        /* optional: logs and continues if absent */
    stats_init();
    fields_init();
    config_load();                       /* NVS; defaults if nothing saved */
    sun_init();                          /* last GPS position for sunrise / sunset */
    devcon_init(inject_key);             /* dev console on the USB port; mirrors the screen from the first frame */
    ESP_ERROR_CHECK(ui_port_init());
    ui_create();
    ui_set_touch(touch_chip_name());
    ui_set_actions(ride_start, enter_usb_mode, apply_backlight);
    ui_set_usb_reboot_cb(usb_msc_leave_and_restart);
    ui_set_power_off_cb(power_off);
    ui_set_end_ride_cb(end_ride);
    ride_init(on_ride_mode);
    vTaskDelay(pdMS_TO_TICKS(100));      /* let the first frame render */
    apply_backlight();

    ant_init(on_ant, NULL);
    ant_set_wheel_mm(config_get()->wheel_mm);
    ESP_ERROR_CHECK(nrf_link_init(on_frame, NULL));
    ESP_ERROR_CHECK(gps_init(on_gps, NULL));

    if (sdcard_mount() == ESP_OK) {
        const sdcard_info_t *sd = sdcard_info();
        ui_set_sd(true, sd->name, sd->size_mb);
        mapview_init();                                     /* vendor vector maps in MAP/ */
        route_load(config_get()->route, config_get()->route_reverse);                    /* GPX route shown on the map */
        ui_maps_changed();
    }

    uint32_t last_pwr_ms = 0;
    bool first = true;
    bool sensors_started = false;
    for (;;) {
        uint32_t now = esp_timer_get_time() / 1000;

        /* vendor boot: SendQuerySlaveModeCmd once, then SendPowerOnCmd every
         * second until the nRF acknowledges (state INIT_QUERY) */
        if (!s_nrf_alive && (first || now - last_pwr_ms >= 1000)) {
            if (first) {
                uint8_t q[8] = {0x24, 0x01, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF};
                nrf_link_send(NRF_TYPE_SET, 0x01, q, sizeof q);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            first = false;
            last_pwr_ms = now;
            nrf_link_send_power_on();
            nrf_link_send_gps_power(NRF_GPS_ON);  /* vendor default; harmless if already on */
        }

        /* The nRF tears its ANT channels down after our power-on command and
         * reports its device list every 5 s. A connect sent while that is in
         * progress gets killed at the next 5 s tick, so wait until after the
         * first tick (~6 s) and connect whatever is not live by then. */
        if (s_nrf_alive && !sensors_started && sdcard_info()->mounted && now > 8000) {
            sensors_started = true;
            connect_paired_sensors();
        }

        ui_tick(now / 1000);
        tracklog_tick();                 /* one FIT record per second while riding */

        /* auto pause: wheel sensor speed if live, else GPS ground speed */
        {
            ant_sensors_t v;
            gps_fix_t fix;
            ant_get(&v);
            gps_get(&fix);
            if (ant_live(ANT_DEV_SPEED, ANT_DEV_SPD_CAD)) {
                ride_speed(v.speed_kmh, true);
            } else {
                ride_speed(fix.speed_kmh, fix.valid && now - fix.last_rx_ms < 3000);
            }
        }
        if (sensors_started) {
            size_t nch;
            const ant_channel_t *ch = ant_channels(&nch);
            ant_sensors_t v;
            ant_get(&v);
            ui_set_sensors(&v, ch, nch);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
