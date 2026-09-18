#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* ANT+ device types (as used by the nRF link: frame cmd byte) */
#define ANT_DEV_POWER      11
#define ANT_DEV_FE         17
#define ANT_DEV_SHIFTING   34
#define ANT_DEV_LIGHT      35
#define ANT_DEV_RADAR      40
#define ANT_DEV_HR         120
#define ANT_DEV_SPD_CAD    121
#define ANT_DEV_CADENCE    122
#define ANT_DEV_SPEED      123
#define ANT_DEV_DI2        128   /* vendor: treated as shifting */

typedef enum { ANT_ST_IDLE, ANT_ST_SEARCHING, ANT_ST_CONNECTED, ANT_ST_TIMEOUT } ant_state_t;

typedef struct {
    uint8_t dev_type;
    uint16_t dev_num;
    uint8_t trans_type;
    ant_state_t state;
    uint32_t pages;        /* data pages received */
    uint32_t last_rx_ms;
} ant_channel_t;

typedef struct {
    /* heart rate */
    uint8_t hr_bpm;
    /* cadence / speed from 121/122/123 */
    float cadence_rpm;
    float speed_kmh;       /* uses the configured wheel circumference */
    uint32_t wheel_revs;
    /* power meter page 0x10 */
    uint16_t power_w;
    uint8_t power_cadence;
} ant_sensors_t;

/* Wheel circumference for speed / distance from the wheel sensor. */
void ant_set_wheel_mm(uint16_t mm);
float ant_wheel_m(void);

typedef struct {
    uint8_t dev_type;
    uint16_t dev_num;
    uint8_t trans_type;
    int8_t rssi;
} ant_scan_result_t;

typedef void (*ant_update_cb_t)(const ant_sensors_t *s, void *ctx);
typedef void (*ant_scan_cb_t)(const ant_scan_result_t *r, bool scan_end, void *ctx);

void ant_init(ant_update_cb_t cb, void *ctx);
void ant_set_scan_cb(ant_scan_cb_t cb, void *ctx);

/* Returns true if the frame was an ANT frame (consumed). Call from the nRF
 * link callback. */
bool ant_handle_frame(const uint8_t *frame, size_t len);

esp_err_t ant_connect(uint8_t dev_type, uint16_t dev_num, uint8_t trans_type);

/* Register a channel the nRF already has open (from its status broadcast)
 * without sending anything. */
void ant_track(uint8_t dev_type, uint16_t dev_num, uint8_t trans_type);

/* True once the nRF has reported at least one channel status (its ANT
 * stack is up). ant_nrf_has() tells whether it reported this device type. */
bool ant_nrf_seen_any(void);
bool ant_nrf_has(uint8_t dev_type);
/* True if the nRF reported this device type as connected (status 3). */
bool ant_nrf_live(uint8_t dev_type);
esp_err_t ant_disconnect(uint8_t dev_type);
/* Close the channel and drop it from the list (re-pairing flow). */
void ant_forget(uint8_t dev_type);
esp_err_t ant_scan(uint16_t seconds);   /* 0 = stop */

const ant_channel_t *ant_channels(size_t *count);
/* True when a channel of device type a or b is connected and delivered a
 * page in the last ANT_LIVE_MS (i.e. its values are current). */
#define ANT_LIVE_MS 10000
bool ant_live(uint8_t dev_type_a, uint8_t dev_type_b);
void ant_get(ant_sensors_t *out);
const char *ant_dev_name(uint8_t dev_type);
