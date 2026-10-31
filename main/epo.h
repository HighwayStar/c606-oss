#pragma once
#include <stddef.h>
#include <stdint.h>

#define EPO_RECORD_SIZE 72
#define EPO_GPS_RECORDS 32
#define EPO_GPS_SET_SIZE (EPO_RECORD_SIZE * EPO_GPS_RECORDS)

/* Magene's six-member AGNSS archive. Copies a validated GPS set for current
 * UTC (6-hour window), including XOR checksums and unique satellite IDs.
 * Returns 1 on success, 0 if expired/not yet valid, -1 if malformed. */
int epo_select_gps(const uint8_t *archive, size_t size, uint32_t utc,
                   uint8_t out[EPO_GPS_SET_SIZE], uint32_t *expires);
int epo_record_valid(const uint8_t record[EPO_RECORD_SIZE]);
/* PAIR471 Host EPO (RAM only), PRN and words in hexadecimal as specified. */
size_t epo_record_frame(const uint8_t record[EPO_RECORD_SIZE], char *out, size_t capacity);
