/* Ride history, see history.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include "esp_log.h"

#include "history.h"
#include "tracklog.h"
#include "route.h"
#include "config.h"
#include "utc.h"

static const char *TAG = "history";

#define VENDOR_DIR  "FITS/FIT"          /* relative to the card */
#define SCAN_MAX    256                 /* rides considered before sorting */

static bool ends_with_fit(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".fit") == 0;
}

/* "YYYYMMDD-HHMMSS.fit" (local time) -> unix; 0 when the name is something else */
static uint32_t parse_ours(const char *n)
{
    if (strlen(n) != 19 || n[8] != '-') return 0;
    for (int i = 0; i < 15; i++) {
        if (i != 8 && !isdigit((unsigned char)n[i])) return 0;
    }
    int y = atoi((char[]){ n[0], n[1], n[2], n[3], 0 });
    int mo = (n[4] - '0') * 10 + n[5] - '0', d = (n[6] - '0') * 10 + n[7] - '0';
    int hh = (n[9] - '0') * 10 + n[10] - '0', mm = (n[11] - '0') * 10 + n[12] - '0', ss = (n[13] - '0') * 10 + n[14] - '0';
    if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 59) return 0;
    return utc_from_civil(y, mo, d, hh, mm, ss) - config_get()->tz_min * 60;
}

/* "<unix seconds>.fit" -> unix; 0 otherwise */
static uint32_t parse_vendor(const char *n)
{
    size_t l = strlen(n) - 4;
    if (l < 9 || l > 10) return 0;
    for (size_t i = 0; i < l; i++) if (!isdigit((unsigned char)n[i])) return 0;
    return (uint32_t)strtoul(n, NULL, 10);
}

static int cmp_newest(const void *a, const void *b)
{
    const history_entry_t *x = a, *y = b;
    return x->start_unix < y->start_unix ? 1 : x->start_unix > y->start_unix ? -1 : strcmp(y->name, x->name);
}

static int scan_dir(const char *dir, bool vendor, history_entry_t *out, int n, int max)
{
    DIR *d = opendir(dir);
    if (!d) return n;
    const char *recording = tracklog_active() ? strrchr(tracklog_filename(), '/') : NULL;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        if (!ends_with_fit(e->d_name)) continue;
        if (recording && !strcmp(e->d_name, recording + 1)) continue;
        history_entry_t *h = &out[n];
        int len = vendor ? snprintf(h->name, sizeof h->name, VENDOR_DIR "/%s", e->d_name)
                         : snprintf(h->name, sizeof h->name, "%s", e->d_name);
        if (len >= (int)sizeof h->name) continue;
        h->vendor = vendor;
        h->start_unix = vendor ? parse_vendor(e->d_name) : parse_ours(e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

int history_list(history_entry_t *out, int max, int *total)
{
    history_entry_t *all = calloc(SCAN_MAX, sizeof *all);
    if (!all) return 0;
    int n = scan_dir(TRACKLOG_DIR, false, all, 0, SCAN_MAX);
    n = scan_dir(SD_MOUNT_POINT "/" VENDOR_DIR, true, all, n, SCAN_MAX);
    qsort(all, n, sizeof *all, cmp_newest);
    if (total) *total = n;
    if (n > max) n = max;
    memcpy(out, all, n * sizeof *all);
    free(all);
    return n;
}

void history_label(const history_entry_t *e, char *buf, size_t n)
{
    if (!e->start_unix) {
        const char *base = strrchr(e->name, '/');
        snprintf(buf, n, "%s", base ? base + 1 : e->name);
        return;
    }
    time_t local = (time_t)e->start_unix + config_get()->tz_min * 60;
    struct tm tm;
    gmtime_r(&local, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

esp_err_t history_delete(const char *name)
{
    char path[ROUTE_PATH_MAX];
    route_path(name, path, sizeof path);
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "cannot delete %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted %s", path);
    return ESP_OK;
}
