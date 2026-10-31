#include "epo.h"
#include <stdio.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int epo_record_valid(const uint8_t record[EPO_RECORD_SIZE])
{
    if (!record || record[3] < 1 || record[3] > 32) return 0;
    uint32_t checksum = 0;
    for (unsigned i = 0; i < 17; i++) checksum ^= le32(record + i * 4);
    return checksum == le32(record + 68);
}

int epo_select_gps(const uint8_t *archive, size_t size, uint32_t utc,
                   uint8_t out[EPO_GPS_SET_SIZE], uint32_t *expires)
{
    /* Observed vendor archive: count, six 28-byte-name/uint32-size entries,
     * reserved bytes, version=1, magic; payload starts at byte 224. */
    if (!archive || !out || !expires || size < 224 || archive[0] != 6 ||
        le32(archive + 216) != 1 || le32(archive + 220) != 0xaa555aa5) return -1;
    size_t offset = 224;
    const uint8_t *selected = NULL;
    uint32_t selected_start = 0, selected_end = 0;
    unsigned seen = 0;
    for (unsigned file = 0; file < 6; file++) {
        const uint8_t *entry = archive + 1 + file * 32;
        if (!memchr(entry, 0, 28)) return -1;
        size_t length = le32(entry + 28);
        if (!length || length > size - offset) return -1;
        if (file == 0) {
            if (strcmp((const char *)entry, "EPO_BDS_3.DAT")) return -1;
        } else {
            char name[28];
            snprintf(name, sizeof name, "EPO_GPS_3_%u.DAT", file);
            if (strcmp((const char *)entry, name) || length != 12 * EPO_GPS_SET_SIZE) return -1;
            for (size_t set = 0; set < length; set += EPO_GPS_SET_SIZE) {
                const uint8_t *data = archive + offset + set;
                uint32_t hour = le32(data) & 0xffffff;
                uint32_t satellites = 0;
                for (unsigned sv = 0; sv < 32; sv++) {
                    const uint8_t *record = data + sv * EPO_RECORD_SIZE;
                    if (!epo_record_valid(record) || (le32(record) & 0xffffff) != hour) return -1;
                    uint32_t bit = 1u << (record[3] - 1);
                    if (satellites & bit) return -1;
                    satellites |= bit;
                }
                uint64_t start = 315964800ULL + (uint64_t)hour * 3600;
                uint64_t end = start + 6 * 3600;
                /* Keep a two-minute margin for GPS/UTC epoch offsets and
                 * transfer time. Never inject data near expiry. */
                if (start <= utc && (uint64_t)utc + 120 < end && end <= UINT32_MAX &&
                    (!selected || start > selected_start)) {
                    selected = data;
                    selected_start = (uint32_t)start;
                    selected_end = (uint32_t)end;
                }
            }
            seen++;
        }
        offset += length;
    }
    if (offset != size || seen != 5) return -1;
    if (!selected) return 0;
    memcpy(out, selected, EPO_GPS_SET_SIZE);
    *expires = selected_end;
    return 1;
}

size_t epo_record_frame(const uint8_t record[EPO_RECORD_SIZE], char *out, size_t capacity)
{
    if (!out || capacity < 192 || !epo_record_valid(record)) return 0;
    size_t n = snprintf(out, capacity, "$PAIR471,0,%X", record[3]);
    for (unsigned word = 0; word < 18; word++)
        n += snprintf(out + n, capacity - n, ",%lX", (unsigned long)le32(record + word * 4));
    uint8_t checksum = 0;
    for (size_t i = 1; i < n; i++) checksum ^= (uint8_t)out[i];
    n += snprintf(out + n, capacity - n, "*%02X\r\n", checksum);
    return n;
}
