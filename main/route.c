/* GPX route loader, see route.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "route.h"
#include "mapfile.h"

static const char *TAG = "route";

static SemaphoreHandle_t s_mtx;
static int32_t *s_x, *s_y;             /* zoom-20 pixels, PSRAM */
static size_t s_n;
static char s_name[ROUTE_NAME_MAX];
static float s_len_m;
static uint32_t s_gen;
static int32_t s_min_lat, s_min_lon, s_max_lat, s_max_lon;

static void lock(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_mtx); }

void route_lock(void) { lock(); }
void route_unlock(void) { unlock(); }

/* ---- parser ------------------------------------------------------------- */

#define CHUNK 4096
#define CARRY 2048     /* longest <trkpt>...</trkpt> element handled in one piece */
#define ELE_HYST_M 5.0 /* elevation change that counts towards climb / descent */

typedef struct {
    int32_t *x, *y;
    size_t n, cap;
    unsigned stride, skip;     /* decimation: keep one point in `stride` */
    uint32_t total;
    double last_lat, last_lon;
    double len_m;
    int32_t min_lat, min_lon, max_lat, max_lon;
    bool has_ele;
    double ele_ref, climb_m, descent_m;
    float min_ele, max_ele;
} build_t;

static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * M_PI / 180.0, dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) * sin(dlon / 2) * sin(dlon / 2);
    return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static void add_point(build_t *b, double lat, double lon)
{
    if (b->total) b->len_m += haversine_m(b->last_lat, b->last_lon, lat, lon);
    b->last_lat = lat;
    b->last_lon = lon;
    int32_t ulat = (int32_t)(lat * 1e6), ulon = (int32_t)(lon * 1e6);
    if (!b->total) { b->min_lat = b->max_lat = ulat; b->min_lon = b->max_lon = ulon; }
    if (ulat < b->min_lat) b->min_lat = ulat;
    if (ulat > b->max_lat) b->max_lat = ulat;
    if (ulon < b->min_lon) b->min_lon = ulon;
    if (ulon > b->max_lon) b->max_lon = ulon;
    b->total++;

    if (b->skip++ % b->stride) return;
    if (b->n == b->cap) {
        /* full: keep every other point and halve the input rate from now on */
        for (size_t i = 0; i < b->n / 2; i++) { b->x[i] = b->x[2 * i]; b->y[i] = b->y[2 * i]; }
        b->n /= 2;
        b->stride *= 2;
    }
    b->x[b->n] = (int32_t)mapfile_lon_to_px(lon, 20);
    b->y[b->n] = (int32_t)mapfile_lat_to_py(lat, 20);
    b->n++;
}

/* Climb / descent with a hysteresis: a change only counts once the
 * elevation has moved ELE_HYST_M away from the last counted level. */
static void add_ele(build_t *b, double ele)
{
    if (!b->has_ele) {
        b->has_ele = true;
        b->ele_ref = ele;
        b->min_ele = b->max_ele = (float)ele;
        return;
    }
    if (ele < b->min_ele) b->min_ele = (float)ele;
    if (ele > b->max_ele) b->max_ele = (float)ele;
    double d = ele - b->ele_ref;
    if (d >= ELE_HYST_M) { b->climb_m += d; b->ele_ref = ele; }
    else if (d <= -ELE_HYST_M) { b->descent_m -= d; b->ele_ref = ele; }
}

/* Scans the text for <trkpt ...> / <rtept ...> elements: lat/lon
 * attributes and an <ele> child. The unfinished tail is kept for the next
 * chunk (`eof`: nothing more is coming, take what is there). */
static void scan(build_t *b, char *buf, size_t *len, bool eof)
{
    char *p = buf, *end = buf + *len;
    for (;;) {
        char *tag = memmem(p, end - p, "<trkpt", 6);
        char *tag2 = memmem(p, end - p, "<rtept", 6);
        if (!tag || (tag2 && tag2 < tag)) tag = tag2;
        if (!tag) { p = end; break; }
        char *close = memchr(tag, '>', end - tag);
        if (!close) { p = tag; break; }          /* tag continues in the next chunk */
        char *next = close + 1;
        char *ele = NULL;
        if (close[-1] != '/') {                  /* not self-closing: look for the element end */
            const char *etag = tag[1] == 't' ? "</trkpt>" : "</rtept>";
            char *cend = memmem(next, end - next, etag, 8);
            if (!cend && !eof && end - tag < CARRY) { p = tag; break; }   /* wait for the rest */
            if (cend) {
                ele = memmem(next, cend - next, "<ele>", 5);
                next = cend + 8;
            }
        }
        *close = 0;
        char *la = strstr(tag, "lat=\""), *lo = strstr(tag, "lon=\"");
        if (la && lo) {
            add_point(b, atof(la + 5), atof(lo + 5));
            if (ele) add_ele(b, atof(ele + 5));
        }
        p = next;
    }
    /* keep the unfinished tail (at most CARRY, at least enough for a split
     * "<trkpt") for the next chunk */
    size_t rest = end - p;
    if (rest > CARRY) { p = end - CARRY; rest = CARRY; }
    if (rest < 8 && *len >= 8) { p = end - 8; rest = 8; }
    memmove(buf, p, rest);
    *len = rest;
}

/* Parses ROUTE_DIR/name into `b` (x/y/cap set by the caller). */
static esp_err_t parse_file(const char *name, build_t *b)
{
    char path[sizeof ROUTE_DIR + ROUTE_NAME_MAX + 1];
    snprintf(path, sizeof path, ROUTE_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    char *buf = heap_caps_malloc(CHUNK + CARRY, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    b->stride = 1;
    size_t len = 0;
    for (;;) {
        size_t got = fread(buf + len, 1, CHUNK, f);
        if (!got) break;
        len += got;
        scan(b, buf, &len, false);
    }
    scan(b, buf, &len, true);
    fclose(f);
    free(buf);
    if (!b->total) {
        ESP_LOGW(TAG, "%s: no track points", name);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool alloc_points(build_t *b, size_t cap)
{
    b->cap = cap;
    b->x = heap_caps_malloc(cap * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    b->y = heap_caps_malloc(cap * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (b->x && b->y) return true;
    free(b->x); free(b->y);
    b->x = b->y = NULL;
    return false;
}

esp_err_t route_load(const char *name, bool reverse)
{
    route_clear();
    if (!name || !*name) return ESP_OK;

    build_t b = { 0 };
    if (!alloc_points(&b, ROUTE_MAX_POINTS)) return ESP_ERR_NO_MEM;
    esp_err_t err = parse_file(name, &b);
    if (err != ESP_OK) {
        free(b.x); free(b.y);
        return err;
    }
    if (reverse) {
        for (size_t i = 0, j = b.n - 1; i < j; i++, j--) {
            int32_t t = b.x[i]; b.x[i] = b.x[j]; b.x[j] = t;
            t = b.y[i]; b.y[i] = b.y[j]; b.y[j] = t;
        }
    }
    lock();
    s_x = b.x; s_y = b.y; s_n = b.n;
    s_len_m = (float)b.len_m;
    s_min_lat = b.min_lat; s_min_lon = b.min_lon; s_max_lat = b.max_lat; s_max_lon = b.max_lon;
    strncpy(s_name, name, sizeof s_name - 1);
    s_gen++;
    unlock();
    ESP_LOGI(TAG, "%s: %u points (1/%u), %.1f km%s", name, (unsigned)s_n, b.stride, s_len_m / 1000,
             reverse ? ", reversed" : "");
    return ESP_OK;
}

esp_err_t route_scan(const char *name, size_t max_points, route_info_t *info)
{
    memset(info, 0, sizeof *info);
    build_t b = { 0 };
    if (!alloc_points(&b, max_points)) return ESP_ERR_NO_MEM;
    esp_err_t err = parse_file(name, &b);
    if (err != ESP_OK) {
        free(b.x); free(b.y);
        return err;
    }
    info->npoints = b.total;
    info->len_m = (float)b.len_m;
    info->has_ele = b.has_ele;
    info->climb_m = (float)b.climb_m;
    info->descent_m = (float)b.descent_m;
    info->min_ele_m = b.min_ele;
    info->max_ele_m = b.max_ele;
    info->min_lat = b.min_lat; info->min_lon = b.min_lon; info->max_lat = b.max_lat; info->max_lon = b.max_lon;
    info->x20 = b.x; info->y20 = b.y; info->n = b.n;
    ESP_LOGI(TAG, "%s: %lu points, %.1f km, %s+%.0f/-%.0f m", name, (unsigned long)b.total, b.len_m / 1000,
             b.has_ele ? "" : "no elevation ", b.climb_m, b.descent_m);
    return ESP_OK;
}

void route_info_free(route_info_t *info)
{
    free(info->x20); free(info->y20);
    info->x20 = info->y20 = NULL;
    info->n = 0;
}

void route_clear(void)
{
    lock();
    free(s_x); free(s_y);
    s_x = s_y = NULL;
    s_n = 0;
    s_name[0] = 0;
    s_len_m = 0;
    s_gen++;
    unlock();
}

bool route_loaded(void) { return s_n > 0; }
const char *route_name(void) { return s_name; }
float route_length_m(void) { return s_len_m; }
uint32_t route_generation(void) { return s_gen; }

size_t route_points(const int32_t **x20, const int32_t **y20)
{
    *x20 = s_x;
    *y20 = s_y;
    return s_n;
}

void route_bbox(int32_t *min_lat, int32_t *min_lon, int32_t *max_lat, int32_t *max_lon)
{
    *min_lat = s_min_lat; *min_lon = s_min_lon; *max_lat = s_max_lat; *max_lon = s_max_lon;
}

int route_list(char names[][ROUTE_NAME_MAX], int max)
{
    DIR *d = opendir(ROUTE_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l < 5 || l >= ROUTE_NAME_MAX || strcasecmp(e->d_name + l - 4, ".gpx") != 0) continue;
        strcpy(names[n++], e->d_name);
    }
    closedir(d);
    return n;
}
