#include "epo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, unsigned long x)
{
    p[0] = x; p[1] = x >> 8; p[2] = x >> 16; p[3] = x >> 24;
}

int main(int argc, char **argv)
{
    const size_t size = 224 + 72 + 5 * 12 * EPO_GPS_SET_SIZE;
    unsigned char *b = calloc(1, size), set[EPO_GPS_SET_SIZE];
    assert(b);
    b[0] = 6;
    put32(b + 216, 1); put32(b + 220, 0xaa555aa5);
    strcpy((char *)b + 1, "EPO_BDS_3.DAT"); put32(b + 29, 72);
    unsigned hour = 410400; /* 2026-10-31 00:00 UTC from downloaded GPS EPO */
    unsigned utc = 315964800u + hour * 3600u + 21 * 3600u;
    size_t off = 296;
    for (unsigned file = 1; file <= 5; file++) {
        snprintf((char *)b + 1 + file * 32, 28, "EPO_GPS_3_%u.DAT", file);
        put32(b + 29 + file * 32, 12 * EPO_GPS_SET_SIZE);
        for (unsigned s = 0; s < 12; s++) for (unsigned sv = 1; sv <= 32; sv++) {
            unsigned char *record = b + off + s * EPO_GPS_SET_SIZE + (sv - 1) * 72;
            unsigned w = hour + ((file - 1) * 12 + s) * 6 + (sv << 24);
            put32(record, w); put32(record + 68, w);
        }
        off += 12 * EPO_GPS_SET_SIZE;
    }
    unsigned expires;
    assert(epo_select_gps(b, size, utc, set, &expires) == 1);
    assert(expires == 315964800u + hour * 3600u + 24 * 3600u);
    assert(set[0] == ((hour + 18) & 255) && set[3] == 1 && set[31 * 72 + 3] == 32);
    assert(epo_select_gps(b, size, 1700000000, set, &expires) == 0);
    assert(epo_select_gps(b, size, utc + 20 * 86400u, set, &expires) == 0);
    assert(epo_select_gps(b, size - 1, utc, set, &expires) == -1);
    b[29] = 0xff;
    assert(epo_select_gps(b, size, utc, set, &expires) == -1);
    put32(b + 29, 72);
    b[296 + 12] ^= 1;
    assert(epo_select_gps(b, size, utc, set, &expires) == -1);
    b[296 + 12] ^= 1;
    unsigned char *last = b + 296 + 31 * 72;
    last[3] = 31; last[71] = 31; /* valid XOR but duplicate SV */
    assert(epo_select_gps(b, size, utc, set, &expires) == -1);
    last[3] = 32; last[71] = 32;
    assert(epo_select_gps(b, size, utc, set, &expires) == 1);
    char frame[224];
    size_t n = epo_record_frame(set + 15 * 72, frame, sizeof frame);
    assert(n && !strncmp(frame, "$PAIR471,0,10,", 14)); /* PRN 16 must be hexadecimal */
    assert(!strcmp(frame + n - 2, "\r\n"));
    assert(!epo_record_frame(set, frame, 10));
    free(b);
    if (argc == 2) {
        FILE *f = fopen(argv[1], "rb"); assert(f);
        b = malloc(262144); assert(b);
        size_t nbytes = fread(b, 1, 262144, f); assert(feof(f)); fclose(f);
        assert(epo_select_gps(b, nbytes, 1793481172, set, &expires) == 1);
        assert(expires == 1793491200); /* 2026-11-01 00:00 UTC */
        free(b);
    }
    puts("PASS: EPO dates, bounds, checksums, duplicate PRNs, hexadecimal protocol and vendor sample");
}
