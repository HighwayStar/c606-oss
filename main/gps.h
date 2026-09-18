#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool valid;            /* RMC status 'A' */
    uint8_t fix_quality;   /* GGA field 6: 0 none, 1 GPS, 2 DGPS, ... */
    uint8_t sats_used;     /* GGA field 7 */
    uint8_t sats_in_view;  /* sum of GSV totals per talker */
    float hdop;
    double lat, lon;       /* degrees, +N/+E */
    float alt_m;
    float speed_kmh;
    float course_deg;
    uint8_t hh, mm, ss;    /* UTC */
    uint8_t day, mon, year; /* year = 2-digit */
    uint32_t sentences;    /* valid NMEA sentences seen */
    uint32_t bad_checksum;
    uint32_t last_rx_ms;   /* esp_timer ms of the last valid sentence */
    uint32_t baud;         /* detected baud rate, 0 while probing */
} gps_fix_t;

typedef void (*gps_update_cb_t)(const gps_fix_t *fix, void *ctx);

/* Starts the UART, probes the baud rate and parses NMEA in its own task.
 * `cb` is invoked (from that task) after every RMC/GGA sentence. */
esp_err_t gps_init(gps_update_cb_t cb, void *ctx);

/* Copy of the latest state. */
void gps_get(gps_fix_t *out);

/* Developer console: feed one NMEA sentence ("$GPRMC,...*hh") through the
 * parser as if it came from the receiver; the UART is ignored while
 * simulating. gps_simulate(false) hands control back to the receiver. */
bool gps_inject(const char *sentence);
void gps_simulate(bool on);
