/*
 * ANT+ sensors through the nRF co-processor.
 *
 * The nRF is only the radio: the ESP32 asks it to open a channel for a
 * given device type/number and gets the raw 8-byte ANT+ data pages back,
 * one frame per page, with the ANT device type in the frame's cmd byte.
 * Reverse engineered from the vendor's AntPageEventHandler / AntSendCmd /
 * StartAntScanDrive / StartAntDisConnectDrive and its LOG/ files.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "board.h"
#include "nrf_link.h"
#include "ant.h"

static const char *TAG = "ant";

#define MAX_CH 8
#define SUB_CHANNEL 0x17      /* cmd 0x01 sub-command: connect/disconnect/events */
#define SUB_SCAN_RESULT 0xF3  /* cmd 0x10, payload[1] == 3 */
#define CONNECT_TIMEOUT_S 25  /* vendor default */

static ant_channel_t s_ch[MAX_CH];
static size_t s_nch;
static ant_sensors_t s_val;
static SemaphoreHandle_t s_lock;
static ant_update_cb_t s_cb;
static void *s_cb_ctx;
static ant_scan_cb_t s_scan_cb;
static void *s_scan_ctx;

/* previous page values for delta computations */
static struct { uint16_t t, revs; bool valid; } s_cad, s_spd;

static bool is_ant_type(uint8_t t)
{
    switch (t) {
    case ANT_DEV_POWER: case ANT_DEV_FE: case ANT_DEV_SHIFTING: case ANT_DEV_LIGHT:
    case ANT_DEV_RADAR: case ANT_DEV_HR: case ANT_DEV_SPD_CAD: case ANT_DEV_CADENCE:
    case ANT_DEV_SPEED: case ANT_DEV_DI2:
        return true;
    default:
        return false;
    }
}

const char *ant_dev_name(uint8_t t)
{
    switch (t) {
    case ANT_DEV_POWER: return "power";
    case ANT_DEV_FE: return "trainer";
    case ANT_DEV_SHIFTING: case ANT_DEV_DI2: return "shifting";
    case ANT_DEV_LIGHT: return "light";
    case ANT_DEV_RADAR: return "radar";
    case ANT_DEV_HR: return "HR";
    case ANT_DEV_SPD_CAD: return "spd+cad";
    case ANT_DEV_CADENCE: return "cadence";
    case ANT_DEV_SPEED: return "speed";
    default: return "?";
    }
}

static ant_channel_t *find_ch(uint8_t dev_type)
{
    for (size_t i = 0; i < s_nch; i++) {
        if (s_ch[i].dev_type == dev_type) return &s_ch[i];
    }
    return NULL;
}

void ant_init(ant_update_cb_t cb, void *ctx)
{
    s_lock = xSemaphoreCreateMutex();
    s_cb = cb;
    s_cb_ctx = ctx;
}

void ant_set_scan_cb(ant_scan_cb_t cb, void *ctx)
{
    s_scan_cb = cb;
    s_scan_ctx = ctx;
}

/* ---- TX ---------------------------------------------------------------- */

esp_err_t ant_connect(uint8_t dev_type, uint16_t dev_num, uint8_t trans_type)
{
    ant_channel_t *c = find_ch(dev_type);
    if (!c) {
        if (s_nch == MAX_CH) return ESP_ERR_NO_MEM;
        c = &s_ch[s_nch++];
    }
    c->dev_type = dev_type;
    c->dev_num = dev_num;
    c->trans_type = trans_type;
    c->state = ANT_ST_SEARCHING;
    /* vendor AntSnd:2,1,P=17 <type> <num lo> <num hi> <trans> 00 19 00 */
    uint8_t p[8] = {SUB_CHANNEL, dev_type, dev_num & 0xFF, dev_num >> 8, trans_type, 0x00, CONNECT_TIMEOUT_S, 0x00};
    ESP_LOGI(TAG, "connect %s %u-%u", ant_dev_name(dev_type), dev_num, trans_type);
    return nrf_link_send(NRF_TYPE_SET, 0x01, p, sizeof p);
}

esp_err_t ant_disconnect(uint8_t dev_type)
{
    /* vendor StartAntDisConnectDrive: 17 <type> 00 00 00 01 00 00.
     * WARNING: on this unit, closing channels at boot left the nRF refusing
     * every subsequent open (immediate status 4, no search) until it was
     * power-cycled (hold key 0). The vendor only closes before re-pairing.
     * Not used by the firmware at the moment. */
    uint8_t p[8] = {SUB_CHANNEL, dev_type, 0, 0, 0, 0x01, 0, 0};
    ant_channel_t *c = find_ch(dev_type);
    if (c) c->state = ANT_ST_IDLE;
    return nrf_link_send(NRF_TYPE_SET, 0x01, p, sizeof p);
}

esp_err_t ant_scan(uint16_t seconds)
{
    /* vendor StartAntScanDrive: E1 02 00 T_lo T_hi FF 00 00 (T = 0: stop) */
    uint8_t p[8] = {0xE1, 0x02, 0x00, seconds & 0xFF, seconds >> 8, 0xFF, 0x00, 0x00};
    ESP_LOGI(TAG, "scan %u s", seconds);
    return nrf_link_send(NRF_TYPE_SET, NRF_CMD_SYS, p, sizeof p);
}

/* ---- RX ---------------------------------------------------------------- */

static void decode_page(uint8_t dev_type, const uint8_t *pg, uint32_t now)
{
    uint8_t page = pg[0] & 0x7F;   /* bit 7 = toggle */
    switch (dev_type) {
    case ANT_DEV_HR:
        /* every HR page: [4..5] beat time, [6] beat count, [7] computed HR */
        s_val.hr_bpm = pg[7];
        break;
    case ANT_DEV_CADENCE:
    case ANT_DEV_SPEED:
    case ANT_DEV_SPD_CAD: {
        /* cadence: [4..5] event time (1/1024 s), [6..7] cumulative revs
         * speed:   same layout on 123; on 121 cadence is [0..3], speed [4..7] */
        const uint8_t *cad = NULL, *spd = NULL;
        if (dev_type == ANT_DEV_CADENCE) cad = pg + 4;
        else if (dev_type == ANT_DEV_SPEED) spd = pg + 4;
        else { cad = pg; spd = pg + 4; }
        if (cad) {
            uint16_t t = cad[0] | (cad[1] << 8), r = cad[2] | (cad[3] << 8);
            if (s_cad.valid) {
                uint16_t dt = t - s_cad.t, dr = r - s_cad.revs;
                if (dt) s_val.cadence_rpm = dr * 60.0f * 1024.0f / dt;
                else if (now - s_cad.t > 0) { /* no new event: keep value, decays below */ }
            }
            s_cad.t = t; s_cad.revs = r; s_cad.valid = true;
        }
        if (spd) {
            uint16_t t = spd[0] | (spd[1] << 8), r = spd[2] | (spd[3] << 8);
            if (s_spd.valid) {
                uint16_t dt = t - s_spd.t, dr = r - s_spd.revs;
                if (dt) s_val.speed_kmh = dr * ANT_WHEEL_CIRC_M * 1024.0f / dt * 3.6f;
                s_val.wheel_revs += dr;
            }
            s_spd.t = t; s_spd.revs = r; s_spd.valid = true;
        }
        break;
    }
    case ANT_DEV_POWER:
        if (page == 0x10) {   /* standard power-only: [3] cadence, [6..7] instant power */
            s_val.power_cadence = pg[3];
            s_val.power_w = pg[6] | (pg[7] << 8);
        }
        break;
    default:
        break;
    }
}

bool ant_handle_frame(const uint8_t *f, size_t len)
{
    if (len < 6 + 8 + 2) return false;
    uint8_t cmd = f[5];
    const uint8_t *p = f + 6;
    uint32_t now = esp_timer_get_time() / 1000;

    if (cmd == 0x01 && p[0] == SUB_CHANNEL) {
        /* channel event: [1] dev type, [2] dev num (low byte), [4] trans type,
         * [5] status: 3 connected, 4 not connected (also sent as a periodic
         * status report every 5 s - NOT a reason to reconnect), 5 search timeout */
        ant_channel_t *c = find_ch(p[1]);
        uint8_t st = p[5];
        if (st != 4) {
            ESP_LOGI(TAG, "%s: %s", ant_dev_name(p[1]),
                     st == 3 ? "connected" : st == 5 ? "search timeout" : "event");
        }
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, p, 8, ESP_LOG_DEBUG);
        if (c) {
            if (st == 3) {
                c->state = ANT_ST_CONNECTED;
            } else if (st == 5) {
                c->state = ANT_ST_TIMEOUT;
                /* vendor retries forever; do the same */
                ant_connect(c->dev_type, c->dev_num, c->trans_type);
            } else if (st == 4 && c->state == ANT_ST_CONNECTED && now - c->last_rx_ms > 3000) {
                c->state = ANT_ST_IDLE;   /* really lost: no pages for a while */
            }
        }
        return true;
    }
    if (cmd == NRF_CMD_SYS && p[0] == SUB_SCAN_RESULT && p[1] == 3) {
        ant_scan_result_t r = {
            .dev_type = p[2], .dev_num = p[3] | (p[4] << 8), .trans_type = p[5], .rssi = (int8_t)p[6],
        };
        bool end = p[7] == 0;
        if (end) ESP_LOGI(TAG, "scan end");
        else ESP_LOGI(TAG, "found %s %u-%u rssi %d", ant_dev_name(r.dev_type), r.dev_num, r.trans_type, r.rssi);
        if (s_scan_cb) s_scan_cb(&r, end, s_scan_ctx);
        return true;
    }
    if (is_ant_type(cmd)) {
        ant_channel_t *c = find_ch(cmd);
        static uint32_t logged, last_page_log;
        if (logged < 20 || now - last_page_log >= 10000) {
            last_page_log = now;
            ESP_LOGI(TAG, "%s page %02x: %02x %02x %02x %02x %02x %02x %02x", ant_dev_name(cmd),
                     p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
        logged++;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (c) {
            c->pages++;
            c->last_rx_ms = now;
            c->state = ANT_ST_CONNECTED;
        }
        decode_page(cmd, p, now);
        ant_sensors_t snap = s_val;
        xSemaphoreGive(s_lock);
        if (s_cb) s_cb(&snap, s_cb_ctx);
        return true;
    }
    return false;
}

const ant_channel_t *ant_channels(size_t *count)
{
    *count = s_nch;
    return s_ch;
}

void ant_get(ant_sensors_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_val;
    xSemaphoreGive(s_lock);
}
