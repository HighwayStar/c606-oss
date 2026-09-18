/*
 * Sunrise / sunset (NOAA solar calculator equations, Meeus low-accuracy
 * series). Good to about a minute, which is all the data field needs.
 * See sun.h.
 */
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

#include "sun.h"
#include "utc.h"
#include "config.h"

static const char *TAG = "sun";
#define NVS_NS  "c606oss"
#define NVS_KEY "sunpos"

static struct { bool valid; double lat, lon; } s_pos;
static struct { bool valid; int32_t lat_e6, lon_e6; } s_saved;   /* what NVS holds */
static struct { bool valid; uint32_t day; double lat, lon; sun_times_t t; } s_cache;

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

/* Floor division of unix seconds into days (handles the negative results
 * of the time-zone shift for dates near 1970, which never occur here, but
 * keeps the arithmetic honest). */
static int64_t floor_div(int64_t a, int64_t b) { return (a >= 0 ? a : a - b + 1) / b; }

void sun_compute(uint32_t day_unix, double lat, double lon, sun_times_t *out)
{
    memset(out, 0, sizeof *out);
    uint32_t utc_midnight = (uint32_t)floor_div(day_unix, 86400) * 86400u;

    /* Julian centuries since J2000 at 12:00 UTC of that day */
    double jd = (utc_midnight + 43200.0) / 86400.0 + 2440587.5;
    double T = (jd - 2451545.0) / 36525.0;

    double L0 = fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);   /* geometric mean longitude */
    double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);                 /* mean anomaly */
    double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);            /* eccentricity */
    double Mr = M * DEG2RAD;
    double C = sin(Mr) * (1.914602 - T * (0.004817 + 0.000014 * T))            /* equation of centre */
             + sin(2 * Mr) * (0.019993 - 0.000101 * T) + sin(3 * Mr) * 0.000289;
    double omega = (125.04 - 1934.136 * T) * DEG2RAD;
    double lambda = (L0 + C - 0.00569 - 0.00478 * sin(omega)) * DEG2RAD;      /* apparent longitude */
    double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    double eps = (eps0 + 0.00256 * cos(omega)) * DEG2RAD;                     /* obliquity, corrected */
    double decl = asin(sin(eps) * sin(lambda));                              /* declination */

    double y = tan(eps / 2) * tan(eps / 2);
    double L0r = L0 * DEG2RAD;
    double eot = 4.0 * RAD2DEG * (y * sin(2 * L0r) - 2 * e * sin(Mr) + 4 * e * y * sin(Mr) * cos(2 * L0r)
                                  - 0.5 * y * y * sin(4 * L0r) - 1.25 * e * e * sin(2 * Mr));   /* minutes */

    /* hour angle at the zenith 90.833 deg (refraction + solar radius) */
    double phi = lat * DEG2RAD;
    double cos_ha = (cos(90.833 * DEG2RAD) - sin(phi) * sin(decl)) / (cos(phi) * cos(decl));
    if (cos_ha >= 1.0) { out->polar_night = true; return; }
    if (cos_ha <= -1.0) { out->polar_day = true; return; }
    double ha = acos(cos_ha) * RAD2DEG;                                        /* degrees */

    double noon_min = 720.0 - 4.0 * lon - eot;                                 /* minutes after 0h UTC */
    out->rise_unix = (uint32_t)((int64_t)utc_midnight + (int64_t)lrint((noon_min - ha * 4.0) * 60.0));
    out->set_unix  = (uint32_t)((int64_t)utc_midnight + (int64_t)lrint((noon_min + ha * 4.0) * 60.0));
}

static void save_position(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    int32_t blob[2] = { (int32_t)lrint(s_pos.lat * 1e6), (int32_t)lrint(s_pos.lon * 1e6) };
    esp_err_t err = nvs_set_blob(h, NVS_KEY, blob, sizeof blob);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_saved.valid = true;
        s_saved.lat_e6 = blob[0];
        s_saved.lon_e6 = blob[1];
        ESP_LOGI(TAG, "position saved %.3f %.3f", s_pos.lat, s_pos.lon);
    } else {
        ESP_LOGW(TAG, "save: %s", esp_err_to_name(err));
    }
}

void sun_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t blob[2];
    size_t len = sizeof blob;
    esp_err_t err = nvs_get_blob(h, NVS_KEY, blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof blob) return;
    s_saved.valid = true;
    s_saved.lat_e6 = blob[0];
    s_saved.lon_e6 = blob[1];
    s_pos.lat = blob[0] / 1e6;
    s_pos.lon = blob[1] / 1e6;
    s_pos.valid = true;
    ESP_LOGI(TAG, "last position %.3f %.3f", s_pos.lat, s_pos.lon);
}

void sun_set_position(double lat, double lon)
{
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return;
    s_pos.lat = lat;
    s_pos.lon = lon;
    s_pos.valid = true;
    /* remember it once we have moved ~10 km from the saved spot (bounded
     * number of flash writes: none on a ride around town) */
    if (!s_saved.valid || fabs(lat - s_saved.lat_e6 / 1e6) > 0.1 || fabs(lon - s_saved.lon_e6 / 1e6) > 0.1) {
        save_position();
    }
}

bool sun_has_position(void) { return s_pos.valid; }

bool sun_is_day(bool *day)
{
    sun_times_t s;
    uint32_t now;
    if (!sun_today(&s) || !utc_now(&now)) return false;
    if (s.polar_day) *day = true;
    else if (s.polar_night) *day = false;
    else *day = now >= s.rise_unix && now < s.set_unix;
    return true;
}

bool sun_today(sun_times_t *out)
{
    uint32_t now;
    if (!s_pos.valid || !utc_now(&now)) return false;
    int32_t tz = config_get()->tz_min * 60;
    /* the local calendar day: compute for the UTC day holding its noon */
    uint32_t local_day = (uint32_t)floor_div((int64_t)now + tz, 86400);
    if (!s_cache.valid || s_cache.day != local_day
        || fabs(s_cache.lat - s_pos.lat) > 0.05 || fabs(s_cache.lon - s_pos.lon) > 0.05) {
        uint32_t local_noon = (uint32_t)((int64_t)local_day * 86400 - tz + 43200);
        sun_compute(local_noon, s_pos.lat, s_pos.lon, &s_cache.t);
        s_cache.day = local_day;
        s_cache.lat = s_pos.lat;
        s_cache.lon = s_pos.lon;
        s_cache.valid = true;
    }
    *out = s_cache.t;
    return true;
}
