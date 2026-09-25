#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "utc.h"
#include "gps.h"

/* Written by the nRF task every ~200 ms, read from any task on either core:
 * every access goes through s_mux. `gps_off` is the RTC's offset to GPS time. */
static struct { bool valid; uint32_t unix_s; uint32_t at_ms; int32_t gps_off; } s_rtc;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

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
    uint32_t t = utc_from_civil(1900 + yy1900, mon0 + 1, dd, hh, mm, ss);
    uint32_t now = now_ms();
    taskENTER_CRITICAL(&s_mux);
    if (s_rtc.valid) {
        /* the RTC itself was set (app, vendor fw): the learned offset is void */
        int32_t jump = (int32_t)(t - (s_rtc.unix_s + (now - s_rtc.at_ms) / 1000));
        if (jump > 2 || jump < -2) s_rtc.gps_off = 0;
    }
    s_rtc.unix_s = t;
    s_rtc.at_ms = now;
    s_rtc.valid = true;
    taskEXIT_CRITICAL(&s_mux);
}

static bool gps_date_ok(const gps_fix_t *g)
{
    return g->year >= 20 && g->mon >= 1 && g->mon <= 12 && g->day >= 1 && g->day <= 31 &&
           g->hh <= 23 && g->mm <= 59 && g->ss <= 60;
}

bool utc_now(uint32_t *unix_s)
{
    gps_fix_t g;
    gps_get(&g);
    uint32_t now = now_ms();
    bool fresh_fix = g.valid && gps_date_ok(&g) && (int32_t)(now - g.last_rx_ms) < 2000;
    uint32_t gps = fresh_fix ? utc_from_civil(g.year, g.mon, g.day, g.hh, g.mm, g.ss) : 0;

    taskENTER_CRITICAL(&s_mux);
    bool valid = s_rtc.valid;
    if (valid) {
        /* The nRF RTC can be far off (seen: 79 min on a never-activated
         * CC700 Pro) and we can't write it yet: once GPS has a fix, keep
         * the RTC's offset to GPS time and apply it. */
        int32_t el = (int32_t)(now - s_rtc.at_ms);   /* at_ms may be newer than `now` */
        uint32_t rtc = s_rtc.unix_s + (el > 0 ? (uint32_t)el / 1000 : 0);
        if (fresh_fix) {
            int32_t d = (int32_t)(gps - rtc) - s_rtc.gps_off;
            if (d > 2 || d < -2) s_rtc.gps_off = (int32_t)(gps - rtc);   /* ignore 1 s jitter */
        }
        *unix_s = rtc + s_rtc.gps_off;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (valid) return true;

    /* RMC carries the date as soon as the receiver tracks satellites, before a fix */
    if (g.sentences && gps_date_ok(&g)) {
        int32_t el = (int32_t)(now - g.last_rx_ms);
        *unix_s = utc_from_civil(g.year, g.mon, g.day, g.hh, g.mm, g.ss) + (el > 0 ? (uint32_t)el / 1000 : 0);
        return true;
    }
    return false;
}
