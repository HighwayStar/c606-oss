/* FIT activity reader, see fitread.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "fitread.h"

static const char *TAG = "fitread";

#define MAX_LOCAL   16
#define MAX_FIELDS  48        /* per definition (the vendor's bike_profile has 32) */
#define DATA_BUF    512       /* bytes of a data message kept for decoding; the rest is skipped */
#define IO_BUF      4096
#define ASCENT_HYST 5.0f      /* metres: altitude noise below this does not count as climb */
#define ALT_UNKNOWN -9999.0f

/* global message numbers */
#define MESG_FILE_ID  0
#define MESG_SESSION  18
#define MESG_LAP      19
#define MESG_RECORD   20
#define MESG_EVENT    21

typedef struct { uint8_t num, size, type; } fdef_t;
typedef struct {
    bool used, big;
    uint16_t global, size;
    uint8_t nfields;
    fdef_t f[MAX_FIELDS];
} ldef_t;

/* what the records add up to, for files without a session message */
typedef struct {
    uint32_t first_ts, last_ts;
    float last_dist, max_speed;
    uint32_t hr_sum, hr_n, hr_max, cad_sum, cad_n, cad_max, pwr_sum, pwr_n, pwr_max;
    bool has_alt;
    float alt_min, alt_max, alt_ref, ascent, descent;
    uint32_t timer_ms, timer_start;   /* from timer start / stop events */
    bool timer_running;
    int laps;
} derived_t;

typedef struct {
    FILE *f;
    ldef_t local[MAX_LOCAL];
    uint8_t buf[DATA_BUF];
    fitread_record_cb cb;
    void *ctx;
    fit_summary_t *sum;
    derived_t d;
    fit_summary_t lap;           /* the last lap message: the vendor's files have no session */
} reader_t;

static bool rd(reader_t *r, void *dst, size_t n) { return fread(dst, 1, n, r->f) == n; }

/* element size of a base type (0 for unknown) */
static uint8_t elem_size(uint8_t type)
{
    static const uint8_t k[] = { 1, 1, 1, 2, 2, 4, 4, 1, 4, 8, 1, 2, 4, 1, 8, 8, 8 };
    type &= 0x1F;
    return type < sizeof k ? k[type] : 0;
}

/* First element of a field as a raw integer; false when it holds the base
 * type's "invalid" value. */
static bool raw_value(const uint8_t *p, const fdef_t *fd, bool big, int64_t *out)
{
    uint8_t es = elem_size(fd->type), t = fd->type & 0x1F;
    if (!es || es > 4 || fd->size < es || t == 7 || t == 8) return false;   /* strings / floats unused */
    uint32_t v = 0;
    for (int i = 0; i < es; i++) v |= (uint32_t)p[big ? i : es - 1 - i] << (8 * (es - 1 - i));
    bool z = t >= 10 && t <= 12;                         /* uint8z .. uint32z: 0 is invalid */
    bool s = t == 1 || t == 3 || t == 5;                 /* signed */
    uint32_t inv = es == 1 ? 0xFF : es == 2 ? 0xFFFF : 0xFFFFFFFFu;
    if (s) inv >>= 1;
    if (z ? v == 0 : v == inv) return false;
    if (s) {
        int64_t sv = v;
        if (v & (1u << (8 * es - 1))) sv -= (int64_t)1 << (8 * es);
        *out = sv;
    } else {
        *out = v;
    }
    return true;
}

static void summary_init(fit_summary_t *s)
{
    memset(s, 0, sizeof *s);
    s->distance_m = s->avg_speed_ms = s->max_speed_ms = -1;
    s->avg_hr = s->max_hr = s->avg_cad = s->max_cad = s->avg_power = s->max_power = -1;
    s->ascent_m = s->descent_m = s->calories = -1;
    s->max_alt_m = s->min_alt_m = ALT_UNKNOWN;
}

static void derived_add(derived_t *d, const fit_rec_t *rec)
{
    if (rec->timestamp) {
        if (!d->first_ts) d->first_ts = rec->timestamp;
        d->last_ts = rec->timestamp;
    }
    if (rec->distance_m >= 0) d->last_dist = rec->distance_m;
    if (rec->speed_ms > d->max_speed) d->max_speed = rec->speed_ms;
    if (rec->hr > 0) { d->hr_sum += rec->hr; d->hr_n++; if ((uint32_t)rec->hr > d->hr_max) d->hr_max = rec->hr; }
    if (rec->cadence > 0) { d->cad_sum += rec->cadence; d->cad_n++; if ((uint32_t)rec->cadence > d->cad_max) d->cad_max = rec->cadence; }
    if (rec->power >= 0) { d->pwr_sum += rec->power; d->pwr_n++; if ((uint32_t)rec->power > d->pwr_max) d->pwr_max = rec->power; }
    if (rec->has_alt) {
        if (!d->has_alt) {
            d->has_alt = true;
            d->alt_min = d->alt_max = d->alt_ref = rec->alt_m;
        } else {
            if (rec->alt_m < d->alt_min) d->alt_min = rec->alt_m;
            if (rec->alt_m > d->alt_max) d->alt_max = rec->alt_m;
            float dd = rec->alt_m - d->alt_ref;
            if (dd >= ASCENT_HYST) { d->ascent += dd; d->alt_ref = rec->alt_m; }
            else if (dd <= -ASCENT_HYST) { d->descent -= dd; d->alt_ref = rec->alt_m; }
        }
    }
}

static void decode_record(reader_t *r, const ldef_t *ld, uint32_t ts_override)
{
    fit_rec_t rec = { .speed_ms = -1, .distance_m = -1, .hr = -1, .cadence = -1, .power = -1, .temp_c = INT_MIN };
    int64_t lat = 0, lon = 0;
    bool has_lat = false, has_lon = false;
    const uint8_t *p = r->buf;
    for (int i = 0; i < ld->nfields; i++) {
        const fdef_t *fd = &ld->f[i];
        int64_t v;
        if (p + fd->size > r->buf + DATA_BUF) break;
        if (raw_value(p, fd, ld->big, &v)) {
            switch (fd->num) {
            case 253: rec.timestamp = (uint32_t)v; break;
            case 0: lat = v; has_lat = true; break;
            case 1: lon = v; has_lon = true; break;
            case 2: if (!rec.has_alt) { rec.alt_m = v / 5.0f - 500.0f; rec.has_alt = true; } break;
            case 78: rec.alt_m = v / 5.0f - 500.0f; rec.has_alt = true; break;   /* enhanced_altitude wins */
            case 5: rec.distance_m = v / 100.0f; break;
            case 6: if (rec.speed_ms < 0) rec.speed_ms = v / 1000.0f; break;
            case 73: rec.speed_ms = v / 1000.0f; break;
            case 7: rec.power = (int)v; break;
            case 3: rec.hr = (int)v; break;
            case 4: rec.cadence = (int)v; break;
            case 13: rec.temp_c = (int)v; break;
            default: break;
            }
        }
        p += fd->size;
    }
    if (ts_override) rec.timestamp = ts_override;
    if (has_lat && has_lon) {
        rec.lat = lat * (180.0 / 2147483648.0);
        rec.lon = lon * (180.0 / 2147483648.0);
        rec.has_pos = true;
    }
    if (r->sum) {
        r->sum->records++;
        derived_add(&r->d, &rec);
    }
    if (r->cb) r->cb(&rec, r->ctx);
}

/* Summary fields of the session (global 18) and lap (19) messages: the
 * same quantities under different field numbers. */
enum { F_END, F_START, F_ELAPSED, F_TIMER, F_DIST, F_CAL, F_AVGSPD, F_MAXSPD, F_EAVGSPD, F_EMAXSPD,
       F_AVGHR, F_MAXHR, F_AVGCAD, F_MAXCAD, F_AVGPWR, F_MAXPWR, F_ASC, F_DESC, F_LAPS, F_MAXALT, F_MINALT, F_COUNT };
static const uint8_t k_session_num[F_COUNT] = { 253, 2, 7, 8, 9, 11, 14, 15, 124, 125, 16, 17, 18, 19, 20, 21, 22, 23, 26, 50, 71 };
static const uint8_t k_lap_num[F_COUNT]     = { 253, 2, 7, 8, 9, 11, 13, 14, 110, 111, 15, 16, 17, 18, 19, 20, 21, 22, 0xFE, 43, 62 };

static void decode_summary(reader_t *r, const ldef_t *ld, const uint8_t *nums, fit_summary_t *s)
{
    const uint8_t *p = r->buf;
    for (int i = 0; i < ld->nfields; i++) {
        const fdef_t *fd = &ld->f[i];
        int64_t v;
        if (p + fd->size > r->buf + DATA_BUF) break;
        int what = -1;
        for (int k = 0; k < F_COUNT; k++) if (nums[k] == fd->num) { what = k; break; }
        if (what >= 0 && raw_value(p, fd, ld->big, &v)) {
            switch (what) {
            case F_END: s->end_time = (uint32_t)v; break;
            case F_START: s->start_time = (uint32_t)v; break;
            case F_ELAPSED: s->elapsed_ms = (uint32_t)v; break;
            case F_TIMER: s->timer_ms = (uint32_t)v; break;
            case F_DIST: s->distance_m = v / 100.0f; break;
            case F_CAL: s->calories = (int)v; break;
            case F_AVGSPD: if (s->avg_speed_ms < 0) s->avg_speed_ms = v / 1000.0f; break;
            case F_EAVGSPD: s->avg_speed_ms = v / 1000.0f; break;
            case F_MAXSPD: if (s->max_speed_ms < 0) s->max_speed_ms = v / 1000.0f; break;
            case F_EMAXSPD: s->max_speed_ms = v / 1000.0f; break;
            case F_AVGHR: s->avg_hr = (int)v; break;
            case F_MAXHR: s->max_hr = (int)v; break;
            case F_AVGCAD: s->avg_cad = (int)v; break;
            case F_MAXCAD: s->max_cad = (int)v; break;
            case F_AVGPWR: s->avg_power = (int)v; break;
            case F_MAXPWR: s->max_power = (int)v; break;
            case F_ASC: s->ascent_m = (int)v; break;
            case F_DESC: s->descent_m = (int)v; break;
            case F_LAPS: s->laps = (int)v; break;
            case F_MAXALT: s->max_alt_m = v / 5.0f - 500.0f; break;
            case F_MINALT: s->min_alt_m = v / 5.0f - 500.0f; break;
            default: break;
            }
        }
        p += fd->size;
    }
}

static void decode_file_id(reader_t *r, const ldef_t *ld)
{
    const uint8_t *p = r->buf;
    for (int i = 0; i < ld->nfields; i++) {
        const fdef_t *fd = &ld->f[i];
        int64_t v;
        if (p + fd->size > r->buf + DATA_BUF) break;
        if (fd->num == 8 && (fd->type & 0x1F) == 7) {
            size_t n = fd->size < sizeof r->sum->product_name - 1 ? fd->size : sizeof r->sum->product_name - 1;
            memcpy(r->sum->product_name, p, n);
            r->sum->product_name[n] = 0;
        } else if (raw_value(p, fd, ld->big, &v)) {
            if (fd->num == 1) r->sum->manufacturer = (uint16_t)v;
            else if (fd->num == 2) r->sum->product = (uint16_t)v;
        }
        p += fd->size;
    }
}

/* timer start / stop events: the time the timer ran, for files without a session */
static void decode_event(reader_t *r, const ldef_t *ld)
{
    const uint8_t *p = r->buf;
    int event = -1, type = -1;
    uint32_t ts = 0;
    for (int i = 0; i < ld->nfields; i++) {
        const fdef_t *fd = &ld->f[i];
        int64_t v;
        if (p + fd->size > r->buf + DATA_BUF) break;
        if (raw_value(p, fd, ld->big, &v)) {
            if (fd->num == 253) ts = (uint32_t)v;
            else if (fd->num == 0) event = (int)v;
            else if (fd->num == 1) type = (int)v;
        }
        p += fd->size;
    }
    derived_t *d = &r->d;
    if (event != 0 || !ts) return;                       /* not a timer event */
    if (type == 0 && !d->timer_running) {                /* start */
        d->timer_running = true;
        d->timer_start = ts;
    } else if ((type == 1 || type == 4) && d->timer_running) {   /* stop / stop_all */
        d->timer_running = false;
        if (ts > d->timer_start) d->timer_ms += (ts - d->timer_start) * 1000;
    }
}

static bool read_definition(reader_t *r, uint8_t hdr)
{
    uint8_t fixed[5];
    if (!rd(r, fixed, sizeof fixed)) return false;
    ldef_t *ld = &r->local[hdr & 0x0F];
    memset(ld, 0, sizeof *ld);
    ld->used = true;
    ld->big = fixed[1] == 1;
    ld->global = ld->big ? (fixed[2] << 8) | fixed[3] : fixed[2] | (fixed[3] << 8);
    uint8_t n = fixed[4];
    for (int i = 0; i < n; i++) {
        uint8_t fd[3];
        if (!rd(r, fd, 3)) return false;
        if (i < MAX_FIELDS) {
            ld->f[i].num = fd[0];
            ld->f[i].size = fd[1];
            ld->f[i].type = fd[2];
            ld->nfields++;
        }
        ld->size += fd[1];
    }
    if (hdr & 0x20) {                                    /* developer fields: only their size matters */
        uint8_t nd;
        if (!rd(r, &nd, 1)) return false;
        for (int i = 0; i < nd; i++) {
            uint8_t fd[3];
            if (!rd(r, fd, 3)) return false;
            ld->size += fd[1];
        }
    }
    return true;
}

static bool read_data(reader_t *r, uint8_t hdr)
{
    uint8_t local = hdr & 0x80 ? (hdr >> 5) & 0x03 : hdr & 0x0F;
    ldef_t *ld = &r->local[local];
    if (!ld->used) return false;
    size_t keep = ld->size < DATA_BUF ? ld->size : DATA_BUF;
    if (!rd(r, r->buf, keep)) return false;
    if (ld->size > keep && fseek(r->f, ld->size - keep, SEEK_CUR) != 0) return false;

    /* compressed timestamp header: low 5 bits of the time, the rest from the
     * previous record (rare; our files and the vendor's never use it) */
    uint32_t ts = 0;
    if (hdr & 0x80) {
        uint32_t prev = r->d.last_ts;
        ts = (prev & ~0x1Fu) | (hdr & 0x1F);
        if (ts < prev) ts += 0x20;
    }
    switch (ld->global) {
    case MESG_RECORD: decode_record(r, ld, ts); break;
    case MESG_SESSION: if (r->sum) { r->sum->has_session = true; decode_summary(r, ld, k_session_num, r->sum); } break;
    case MESG_LAP: r->d.laps++; if (r->sum) { summary_init(&r->lap); decode_summary(r, ld, k_lap_num, &r->lap); } break;
    case MESG_FILE_ID: if (r->sum) decode_file_id(r, ld); break;
    case MESG_EVENT: if (r->sum) decode_event(r, ld); break;
    default: break;
    }
    return true;
}

/* fill what the session did not say from the records */
static void summary_finish(fit_summary_t *s, derived_t *d, const fit_summary_t *lap)
{
    /* no session but one lap covering the ride (the vendor's files): the lap is the summary */
    if (!s->has_session && d->laps == 1) {
        fit_summary_t keep = *s;
        *s = *lap;
        s->records = keep.records;
        s->manufacturer = keep.manufacturer; s->product = keep.product;
        memcpy(s->product_name, keep.product_name, sizeof s->product_name);
    }
    if (d->timer_running && d->last_ts > d->timer_start) d->timer_ms += (d->last_ts - d->timer_start) * 1000;
    if (!s->laps) s->laps = d->laps;
    if (!s->start_time) s->start_time = d->first_ts;
    if (!s->end_time) s->end_time = d->last_ts;
    if (!s->elapsed_ms && d->last_ts > d->first_ts) s->elapsed_ms = (d->last_ts - d->first_ts) * 1000;
    if (!s->timer_ms) s->timer_ms = d->timer_ms ? d->timer_ms : s->elapsed_ms;
    if (s->distance_m < 0 && s->records) s->distance_m = d->last_dist;
    if (s->avg_speed_ms < 0 && s->timer_ms >= 1000 && s->distance_m >= 0) s->avg_speed_ms = s->distance_m / (s->timer_ms / 1000.0f);
    if (s->max_speed_ms < 0 && s->records) s->max_speed_ms = d->max_speed;
    if (s->avg_hr < 0 && d->hr_n) s->avg_hr = d->hr_sum / d->hr_n;
    if (s->max_hr < 0 && d->hr_n) s->max_hr = d->hr_max;
    if (s->avg_cad < 0 && d->cad_n) s->avg_cad = d->cad_sum / d->cad_n;
    if (s->max_cad < 0 && d->cad_n) s->max_cad = d->cad_max;
    if (s->avg_power < 0 && d->pwr_n) s->avg_power = d->pwr_sum / d->pwr_n;
    if (s->max_power < 0 && d->pwr_n) s->max_power = d->pwr_max;
    if (s->ascent_m < 0 && d->has_alt) s->ascent_m = (int)(d->ascent + 0.5f);
    if (s->descent_m < 0 && d->has_alt) s->descent_m = (int)(d->descent + 0.5f);
    if (s->max_alt_m == ALT_UNKNOWN && d->has_alt) s->max_alt_m = d->alt_max;
    if (s->min_alt_m == ALT_UNKNOWN && d->has_alt) s->min_alt_m = d->alt_min;
    s->has_alt = s->max_alt_m != ALT_UNKNOWN && s->min_alt_m != ALT_UNKNOWN;
    /* a zero maximum means the sensor was never there */
    if (s->max_hr == 0) s->avg_hr = s->max_hr = -1;
    if (s->max_cad == 0) s->avg_cad = s->max_cad = -1;
    if (s->max_power == 0) s->avg_power = s->max_power = -1;
}

esp_err_t fitread_file(const char *path, fitread_record_cb cb, void *ctx, fit_summary_t *sum)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    reader_t *r = calloc(1, sizeof *r);
    char *iobuf = malloc(IO_BUF);
    if (!r || !iobuf) {
        free(r); free(iobuf); fclose(f);
        return ESP_ERR_NO_MEM;
    }
    setvbuf(f, iobuf, _IOFBF, IO_BUF);
    r->f = f;
    r->cb = cb;
    r->ctx = ctx;
    r->sum = sum;
    if (sum) summary_init(sum);

    esp_err_t err = ESP_OK;
    uint8_t h[14];
    struct stat st;
    long size = fstat(fileno(f), &st) == 0 ? (long)st.st_size : 0;
    if (!rd(r, h, 12) || (h[0] != 12 && h[0] != 14) || memcmp(h + 8, ".FIT", 4) != 0) {
        ESP_LOGW(TAG, "%s: not a FIT file", path);
        err = ESP_ERR_INVALID_ARG;
    } else {
        if (h[0] == 14) rd(r, h + 12, 2);
        uint32_t data = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
        /* the vendor writes 0 here (and no CRC): take the file size instead */
        long end = data ? (long)h[0] + (long)data : size;
        if (end > size) end = size;
        while (ftell(f) < end) {
            uint8_t hdr;
            if (!rd(r, &hdr, 1)) break;
            bool ok = (hdr & 0xC0) == 0x40 ? read_definition(r, hdr) : read_data(r, hdr);
            if (!ok) {
                ESP_LOGD(TAG, "%s: stopped at %ld", path, ftell(f));
                break;
            }
        }
        if (sum) summary_finish(sum, &r->d, &r->lap);
    }
    fclose(f);
    free(iobuf);
    free(r);
    return err;
}
