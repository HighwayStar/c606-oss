#include "esp_timer.h"
#include "utc.h"
#include "gps.h"

static struct { bool valid; uint32_t unix_s; uint32_t at_ms; } s_rtc;

static uint32_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm) */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

uint32_t utc_from_civil(uint16_t year, uint8_t mon, uint8_t day, uint8_t hh, uint8_t mm, uint8_t ss)
{
    if (year < 100) year += 2000;
    return (uint32_t)days_from_civil(year, mon, day) * 86400u + hh * 3600u + mm * 60u + ss;
}

void utc_set_rtc(uint8_t ss, uint8_t mm, uint8_t hh, uint8_t dd, uint8_t mon0, uint8_t yy1900)
{
    dd &= 0x1f;   /* upper bits carry something else (weekday?) */
    /* a fresh nRF (RTC never set) reports 1900/2000-ish dates: ignore those */
    if (yy1900 < 120 || mon0 > 11 || dd < 1 || dd > 31 || hh > 23 || mm > 59 || ss > 60) return;
    s_rtc.unix_s = utc_from_civil(1900 + yy1900, mon0 + 1, dd, hh, mm, ss);
    s_rtc.at_ms = now_ms();
    s_rtc.valid = true;
}

bool utc_now(uint32_t *unix_s)
{
    if (s_rtc.valid) {
        *unix_s = s_rtc.unix_s + (now_ms() - s_rtc.at_ms) / 1000;
        return true;
    }
    gps_fix_t g;
    gps_get(&g);
    /* RMC carries the date as soon as the receiver tracks satellites, before a fix */
    if (g.sentences && g.year >= 20 && g.mon >= 1 && g.mon <= 12 && g.day >= 1) {
        *unix_s = utc_from_civil(g.year, g.mon, g.day, g.hh, g.mm, g.ss) + (now_ms() - g.last_rx_ms) / 1000;
        return true;
    }
    return false;
}
