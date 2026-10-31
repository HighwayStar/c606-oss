/* Host test, no firmware or GNSS device needed. Uses the manufacturer's
 * published PAIR590 example, UTC date boundaries and buffer guard checks. */
#include "gps_aid_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    char out[96];
    /* Quectel LC76G / Airoha AG3352 AGNSS note: 2019-02-10 09:00:58 UTC. */
    size_t n = gps_aid_time_frame(1549789258, out, sizeof out);
    assert(n == strlen(out));
    assert(!strcmp(out, "$PAIR590,2019,2,10,9,0,58*0B\r\n"));
    /* UTC must be independent of the user's local time zone. */
    setenv("TZ", "UTC-9", 1);
    tzset();
    assert(gps_aid_time_frame(1549789258, out, sizeof out)==n);
    assert(!strcmp(out, "$PAIR590,2019,2,10,9,0,58*0B\r\n"));
    assert(gps_aid_time_frame(1709164800, out, sizeof out));
    assert(strstr(out, "$PAIR590,2024,2,29,0,0,0*"));
    assert(gps_aid_time_frame(1709251200, out, sizeof out));
    assert(strstr(out, "$PAIR590,2024,3,1,0,0,0*"));
    memset(out, 'X', sizeof out);
    assert(!gps_aid_time_frame(1549789258, out, 8));
    assert(out[8]=='X');
    assert(!gps_aid_time_frame(0, out, sizeof out));
    assert(!gps_aid_time_frame(1549789258, NULL, 96));
    assert(!gps_aid_time_frame(1549789258, out, 0));
    assert(gps_aid_time_frame(1549789258, out, n+1)==n);
    assert(!gps_aid_time_frame(1549789258, out, n));
    puts("PASS: documented PAIR590 frame, timezone independence, leap day, bounds");
}
