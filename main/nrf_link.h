#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* A validated frame from the nRF. `frame` points at the 0xA5 sync byte and
 * `len` is the full frame length including the CRC. Called from the link
 * task; keep it short (copy what you need). */
typedef void (*nrf_frame_cb_t)(const uint8_t *frame, size_t len, void *ctx);

/* Decoded button event (cmd 0x10, payload[0] == 0x49). */
typedef struct {
    uint8_t key;    /* payload[1]: 0..2 on the vendor firmware, 0 = "A"/power */
    uint8_t event;  /* payload[6]: 1 = press, 4 = long-press start, ... */
    uint8_t aux;    /* payload[5]: unknown, forwarded by vendor */
} nrf_key_event_t;

esp_err_t nrf_link_init(nrf_frame_cb_t cb, void *ctx);

/* Build [A5][len+4][6F][F1][type][cmd][payload][crc16] and send it. */
esp_err_t nrf_link_send(uint8_t type, uint8_t cmd, const void *payload, uint8_t len);

/* Vendor "E2" control message: E2 grp id 00 00 val 00 00 (8 bytes). */
esp_err_t nrf_link_send_ctrl(uint8_t type, uint8_t grp, uint8_t id, uint8_t val);

/* Same as vendor SendPowerOnCmd(): E2 02 00 00 00 01 00 00, type 2 */
esp_err_t nrf_link_send_power_on(void);

/* Vendor SendPowerOffCmd(): E2 02 00 00 00 00 00 00 - the nRF cuts our power. */
esp_err_t nrf_link_send_power_off(void);

/* GPS power: NRF_GPS_OFF / NRF_GPS_ON / NRF_GPS_RESET */
esp_err_t nrf_link_send_gps_power(uint8_t val);

/* Helper: returns true and fills `ev` if `frame` is a button event. */
bool nrf_link_decode_key(const uint8_t *frame, size_t len, nrf_key_event_t *ev);

uint16_t nrf_link_crc16(const uint8_t *p, size_t n);
