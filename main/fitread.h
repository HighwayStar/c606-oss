#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Minimal FIT activity reader for the ride history: streams a file once,
 * hands every `record` with its position to a callback (the route loader
 * turns them into a track for the map) and collects a summary from the
 * `session` message — or, when the file has none (power lost mid-ride),
 * from the records themselves.
 *
 * Tolerant by design: the vendor's files have a zero data size in the
 * header and no trailing CRC, so the reader goes by the file size, stops
 * quietly at the first byte it cannot make sense of and keeps what it has.
 * Big-endian definitions, developer fields and compressed-timestamp
 * headers are handled; only the fields listed in fitread.c are decoded. */

typedef struct {
    uint32_t timestamp;      /* FIT seconds, 0 = none */
    double lat, lon;         /* degrees, valid when has_pos */
    float alt_m;             /* valid when has_alt */
    float speed_ms;          /* < 0 = none */
    float distance_m;        /* < 0 = none */
    int hr, cadence, power, temp_c;   /* -1 / INT_MIN (temp) = none */
    bool has_pos, has_alt;
} fit_rec_t;

typedef struct {
    bool has_session;        /* summary from the session message (else derived from the records) */
    uint32_t start_time;     /* FIT seconds (unix = + FIT_EPOCH_UNIX) */
    uint32_t end_time;
    uint32_t elapsed_ms, timer_ms;
    float distance_m;        /* < 0 = unknown */
    float avg_speed_ms, max_speed_ms;
    int avg_hr, max_hr, avg_cad, max_cad, avg_power, max_power;   /* -1 = unknown */
    int ascent_m, descent_m, calories;
    float max_alt_m, min_alt_m;
    bool has_alt;
    int laps;                /* lap messages seen */
    uint32_t records;
    uint16_t manufacturer, product;
    char product_name[17];
} fit_summary_t;

typedef void (*fitread_record_cb)(const fit_rec_t *r, void *ctx);

/* Reads `path`. `cb` (optional) gets every record; `sum` (optional) the
 * summary. Fails only when the file cannot be opened or is not a FIT file. */
esp_err_t fitread_file(const char *path, fitread_record_cb cb, void *ctx, fit_summary_t *sum);
