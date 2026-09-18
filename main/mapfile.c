/* Mapsforge binary map reader, see mapfile.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "mapfile.h"

static const char *TAG = "mapfile";

#define MAGIC "mapsforge binary OSM"
#define MAGIC_LEN 20
#define HEADER_MAX 8192
#define DEBUG_TILE_SIG 32
#define DEBUG_INDEX_SIG 16
#define DEBUG_WAY_SIG 32
#define INDEX_ENTRY 5

/* ---- in-memory big-endian / varint parser ------------------------------ */

typedef struct {
    const uint8_t *p, *end;
    bool err;
} pb_t;

static uint8_t pb_u8(pb_t *b)
{
    if (b->p >= b->end) { b->err = true; return 0; }
    return *b->p++;
}

static uint16_t pb_u16(pb_t *b) { uint16_t v = (uint16_t)pb_u8(b) << 8; return v | pb_u8(b); }
static uint32_t pb_u32(pb_t *b) { uint32_t v = (uint32_t)pb_u16(b) << 16; return v | pb_u16(b); }
static uint64_t pb_u64(pb_t *b) { uint64_t v = (uint64_t)pb_u32(b) << 32; return v | pb_u32(b); }

/* VBE-U: 7 bits per byte, little-endian order, high bit = continuation */
static uint32_t pb_vu(pb_t *b)
{
    uint32_t v = 0;
    for (int sh = 0; sh < 35; sh += 7) {
        uint8_t c = pb_u8(b);
        v |= (uint32_t)(c & 0x7F) << sh;
        if (!(c & 0x80)) return v;
    }
    b->err = true;
    return v;
}

/* VBE-S: like VBE-U but the last byte carries 6 bits and the sign in bit 6 */
static int32_t pb_vs(pb_t *b)
{
    uint32_t v = 0;
    for (int sh = 0; sh < 35; sh += 7) {
        uint8_t c = pb_u8(b);
        if (c & 0x80) {
            v |= (uint32_t)(c & 0x7F) << sh;
        } else {
            v |= (uint32_t)(c & 0x3F) << sh;
            return (c & 0x40) ? -(int32_t)v : (int32_t)v;
        }
    }
    b->err = true;
    return 0;
}

/* Length-prefixed UTF-8 string; returns a pointer into the buffer and the
 * length, the caller NUL-terminates in place if it owns the buffer. */
static const uint8_t *pb_str(pb_t *b, uint32_t *len)
{
    uint32_t n = pb_vu(b);
    if (b->err || (size_t)(b->end - b->p) < n) { b->err = true; *len = 0; return NULL; }
    const uint8_t *s = b->p;
    b->p += n;
    *len = n;
    return s;
}

/* ---- buffered sequential file reader for one tile ---------------------- */

typedef struct {
    FILE *f;
    uint8_t buf[512];
    unsigned pos, len;
    uint64_t left;       /* bytes of the tile not yet loaded into buf */
    bool err;
} rd_t;

static bool rd_fill(rd_t *r)
{
    if (r->left == 0) return false;
    size_t want = r->left < sizeof r->buf ? (size_t)r->left : sizeof r->buf;
    size_t got = fread(r->buf, 1, want, r->f);
    if (got == 0) { r->err = true; return false; }
    r->left -= got;
    r->pos = 0;
    r->len = got;
    return true;
}

static uint8_t rd_u8(rd_t *r)
{
    if (r->pos >= r->len && !rd_fill(r)) { r->err = true; return 0; }
    return r->buf[r->pos++];
}

static uint32_t rd_vu(rd_t *r)
{
    uint32_t v = 0;
    for (int sh = 0; sh < 35; sh += 7) {
        uint8_t c = rd_u8(r);
        v |= (uint32_t)(c & 0x7F) << sh;
        if (!(c & 0x80)) return v;
    }
    r->err = true;
    return v;
}

static bool rd_bytes(rd_t *r, uint8_t *dst, size_t n)
{
    while (n) {
        if (r->pos >= r->len && !rd_fill(r)) { r->err = true; return false; }
        size_t k = r->len - r->pos;
        if (k > n) k = n;
        if (dst) { memcpy(dst, r->buf + r->pos, k); dst += k; }
        r->pos += k;
        n -= k;
    }
    return true;
}

static uint64_t rd_remaining(const rd_t *r) { return r->left + (r->len - r->pos); }

/* ---- projection --------------------------------------------------------- */

/* Same formulas as mapsforge MercatorProjection with tile size 256. */
static double map_size(uint8_t zoom) { return (double)(256u << zoom); }

double mapfile_lon_to_px(double lon_deg, uint8_t zoom)
{
    return (lon_deg + 180.0) / 360.0 * map_size(zoom);
}

double mapfile_lat_to_py(double lat_deg, uint8_t zoom)
{
    double s = sin(lat_deg * (M_PI / 180.0));
    return (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * map_size(zoom);
}

double mapfile_px_to_lon(double px, uint8_t zoom)
{
    return 360.0 * (px / map_size(zoom) - 0.5);
}

double mapfile_py_to_lat(double py, uint8_t zoom)
{
    double y = 0.5 - py / map_size(zoom);
    return 90.0 - 360.0 * atan(exp(-y * 2 * M_PI)) / M_PI;
}

static uint32_t lon_to_tx(double lon_deg, uint8_t zoom)
{
    double t = floor(mapfile_lon_to_px(lon_deg, zoom) / 256.0);
    double n = (double)(1u << zoom);
    if (t < 0) t = 0;
    if (t > n - 1) t = n - 1;
    return (uint32_t)t;
}

static uint32_t lat_to_ty(double lat_deg, uint8_t zoom)
{
    double t = floor(mapfile_lat_to_py(lat_deg, zoom) / 256.0);
    double n = (double)(1u << zoom);
    if (t < 0) t = 0;
    if (t > n - 1) t = n - 1;
    return (uint32_t)t;
}

/* Tile origin in microdegrees, truncated the way the writer does it
 * (LatLongUtils.degreesToMicrodegrees = (int)(deg * 1e6)). */
static int32_t tile_origin_lat(uint32_t ty, uint8_t zoom)
{
    return (int32_t)(mapfile_py_to_lat((double)ty * 256.0, zoom) * 1e6);
}

static int32_t tile_origin_lon(uint32_t tx, uint8_t zoom)
{
    return (int32_t)(mapfile_px_to_lon((double)tx * 256.0, zoom) * 1e6);
}

/* ---- header ------------------------------------------------------------- */

static void *scratch_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

static char *dup_str(pb_t *b)
{
    uint32_t n;
    const uint8_t *s = pb_str(b, &n);
    if (!s) return NULL;
    char *d = malloc(n + 1);
    if (!d) { b->err = true; return NULL; }
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static bool read_tag_table(pb_t *b, char **tags, uint8_t *count)
{
    uint16_t n = pb_u16(b);
    if (b->err) return false;
    if (n > MAPFILE_MAX_TAGS) {
        ESP_LOGE(TAG, "%u tags, max %u", n, MAPFILE_MAX_TAGS);
        return false;
    }
    for (uint16_t i = 0; i < n; i++) {
        tags[i] = dup_str(b);
        if (!tags[i]) return false;
        *count = i + 1;
    }
    return true;
}

esp_err_t mapfile_open(mapfile_t *mf, const char *path)
{
    memset(mf, 0, sizeof *mf);
    mf->f = fopen(path, "rb");
    if (!mf->f) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *hdr = malloc(HEADER_MAX);
    if (!hdr) { mapfile_close(mf); return ESP_ERR_NO_MEM; }
    size_t got = fread(hdr, 1, HEADER_MAX, mf->f);
    pb_t b = { hdr, hdr + got, false };
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (got < MAGIC_LEN + 8 || memcmp(hdr, MAGIC, MAGIC_LEN) != 0) {
        ESP_LOGW(TAG, "%s: not a mapsforge file", path);
        goto out;
    }
    b.p += MAGIC_LEN;
    uint32_t header_size = pb_u32(&b);
    if (header_size + MAGIC_LEN + 4 > got) {
        ESP_LOGW(TAG, "%s: header %lu bytes too big", path, (unsigned long)header_size);
        goto out;
    }
    b.end = hdr + MAGIC_LEN + 4 + header_size;

    uint32_t version = pb_u32(&b);
    if (version != 3 && version != 4 && version != 5) {
        ESP_LOGW(TAG, "%s: unsupported version %lu", path, (unsigned long)version);
        goto out;
    }
    pb_u64(&b);                              /* file size */
    pb_u64(&b);                              /* creation date */
    mf->min_lat = (int32_t)pb_u32(&b);
    mf->min_lon = (int32_t)pb_u32(&b);
    mf->max_lat = (int32_t)pb_u32(&b);
    mf->max_lon = (int32_t)pb_u32(&b);
    mf->tile_size = pb_u16(&b);
    uint32_t n;
    const uint8_t *proj = pb_str(&b, &n);
    if (!proj || n != 8 || memcmp(proj, "Mercator", 8) != 0) {
        ESP_LOGW(TAG, "%s: projection is not Mercator", path);
        goto out;
    }
    uint8_t flags = pb_u8(&b);
    mf->debug = flags & 0x80;
    if (flags & 0x40) { pb_u32(&b); pb_u32(&b); }   /* start position */
    if (flags & 0x20) pb_u8(&b);                     /* start zoom */
    if (flags & 0x10) pb_str(&b, &n);                /* language preference */
    if (flags & 0x08) pb_str(&b, &n);                /* comment */
    if (flags & 0x04) pb_str(&b, &n);                /* created by */
    if (b.err) goto corrupt;

    if (!read_tag_table(&b, mf->poi_tags, &mf->n_poi_tags)) goto corrupt;
    if (!read_tag_table(&b, mf->way_tags, &mf->n_way_tags)) goto corrupt;

    mf->n_zoom = pb_u8(&b);
    if (b.err || mf->n_zoom == 0 || mf->n_zoom > MAPFILE_MAX_ZOOM_INTERVALS) {
        ESP_LOGW(TAG, "%s: %u zoom intervals", path, mf->n_zoom);
        goto out;
    }
    for (int i = 0; i < mf->n_zoom; i++) {
        mapfile_zoom_interval_t *z = &mf->zoom[i];
        z->base_zoom = pb_u8(&b);
        z->min_zoom = pb_u8(&b);
        z->max_zoom = pb_u8(&b);
        z->start = pb_u64(&b);
        z->size = pb_u64(&b);
        if (b.err || z->base_zoom > 20) goto corrupt;
        z->tx0 = lon_to_tx(mf->min_lon / 1e6, z->base_zoom);
        uint32_t tx1 = lon_to_tx(mf->max_lon / 1e6, z->base_zoom);
        z->ty0 = lat_to_ty(mf->max_lat / 1e6, z->base_zoom);
        uint32_t ty1 = lat_to_ty(mf->min_lat / 1e6, z->base_zoom);
        z->tw = tx1 - z->tx0 + 1;
        z->th = ty1 - z->ty0 + 1;
    }

    mf->way_buf = scratch_alloc(MAPFILE_WAY_BUF);
    mf->str_buf = scratch_alloc(MAPFILE_STR_BUF);
    mf->lat = scratch_alloc(MAPFILE_MAX_POINTS * sizeof(int32_t));
    mf->lon = scratch_alloc(MAPFILE_MAX_POINTS * sizeof(int32_t));
    if (!mf->way_buf || !mf->str_buf || !mf->lat || !mf->lon) { err = ESP_ERR_NO_MEM; goto out; }

    ESP_LOGI(TAG, "%s: v%lu, bbox %ld,%ld..%ld,%ld, %u way tags, %u intervals (base %u/%u/%u)%s",
             path, (unsigned long)version, (long)mf->min_lat, (long)mf->min_lon,
             (long)mf->max_lat, (long)mf->max_lon, mf->n_way_tags, mf->n_zoom,
             mf->zoom[0].base_zoom, mf->n_zoom > 1 ? mf->zoom[1].base_zoom : 0,
             mf->n_zoom > 2 ? mf->zoom[2].base_zoom : 0, mf->debug ? ", debug" : "");
    err = ESP_OK;
    goto out;

corrupt:
    ESP_LOGW(TAG, "%s: corrupt header", path);
out:
    free(hdr);
    if (err != ESP_OK) mapfile_close(mf);
    return err;
}

void mapfile_close(mapfile_t *mf)
{
    if (mf->f) fclose(mf->f);
    for (int i = 0; i < mf->n_poi_tags; i++) free(mf->poi_tags[i]);
    for (int i = 0; i < mf->n_way_tags; i++) free(mf->way_tags[i]);
    free(mf->way_buf);
    free(mf->str_buf);
    free(mf->lat);
    free(mf->lon);
    memset(mf, 0, sizeof *mf);
}

bool mapfile_contains(const mapfile_t *mf, int32_t lat_ud, int32_t lon_ud)
{
    return mf->f && lat_ud >= mf->min_lat && lat_ud <= mf->max_lat &&
           lon_ud >= mf->min_lon && lon_ud <= mf->max_lon;
}

const mapfile_zoom_interval_t *mapfile_interval(const mapfile_t *mf, uint8_t zoom)
{
    for (int i = 0; i < mf->n_zoom; i++) {
        if (zoom >= mf->zoom[i].min_zoom && zoom <= mf->zoom[i].max_zoom) return &mf->zoom[i];
    }
    return NULL;
}

/* ---- tile data ---------------------------------------------------------- */

/* Tag values embedded in the way data (writer >= 0.13, tag names ending in
 * %b %i %f %h %s). The vendor's tag tables have none; skip them if present. */
static void skip_tag_value(pb_t *b, const char *tag)
{
    size_t n = strlen(tag);
    if (n < 2 || tag[n - 2] != '%') return;
    uint32_t len;
    switch (tag[n - 1]) {
    case 'b': pb_u8(b); break;
    case 'h': pb_u16(b); break;
    case 'i': case 'f': pb_u32(b); break;
    case 's': pb_str(b, &len); break;
    default: break;
    }
}

/* Copies a length-prefixed string into the string scratch buffer
 * (truncated if it does not fit) and returns it NUL-terminated. */
static const char *take_str(mapfile_t *mf, pb_t *b, size_t *used)
{
    uint32_t n;
    const uint8_t *s = pb_str(b, &n);
    if (!s) return NULL;
    size_t room = MAPFILE_STR_BUF - *used;
    if (room < 2) return "";
    if (n > room - 1) n = room - 1;
    char *d = mf->str_buf + *used;
    memcpy(d, s, n);
    d[n] = 0;
    *used += n + 1;
    return d;
}

static esp_err_t parse_way(mapfile_t *mf, uint8_t *rec, size_t len, int32_t olat, int32_t olon,
                           mapfile_way_cb_t cb, void *ctx)
{
    pb_t b = { rec, rec + len, false };
    mapfile_way_t w = { 0 };

    if (mf->debug) b.p += DEBUG_WAY_SIG;
    w.subtile_bitmap = pb_u16(&b);
    uint8_t special = pb_u8(&b);
    w.layer = (int8_t)(special >> 4) - 5;
    w.tag_count = special & 0x0F;
    for (int i = 0; i < w.tag_count; i++) {
        uint32_t id = pb_vu(&b);
        if (id >= mf->n_way_tags) { b.err = true; break; }
        w.tags[i] = id;
        skip_tag_value(&b, mf->way_tags[id]);
    }
    uint8_t flags = pb_u8(&b);
    if (b.err) return ESP_ERR_INVALID_SIZE;

    size_t used = 0;
    if (flags & 0x80) w.name = take_str(mf, &b, &used);
    if (flags & 0x40) w.house_number = take_str(mf, &b, &used);
    if (flags & 0x20) w.ref = take_str(mf, &b, &used);
    int32_t label_dlat = 0, label_dlon = 0;
    if (flags & 0x10) { label_dlat = pb_vs(&b); label_dlon = pb_vs(&b); w.has_label_pos = true; }
    uint32_t n_data_blocks = (flags & 0x08) ? pb_vu(&b) : 1;
    bool double_delta = flags & 0x04;
    if (b.err) return ESP_ERR_INVALID_SIZE;

    for (uint32_t d = 0; d < n_data_blocks; d++) {
        uint32_t n_blocks = pb_vu(&b);
        if (b.err || n_blocks > 0xFFFF) return ESP_ERR_INVALID_SIZE;
        for (uint32_t k = 0; k < n_blocks; k++) {
            uint32_t n = pb_vu(&b);
            if (b.err || n == 0) return ESP_ERR_INVALID_SIZE;
            int32_t lat = olat + pb_vs(&b);
            int32_t lon = olon + pb_vs(&b);
            int32_t dlat = 0, dlon = 0;
            uint32_t kept = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (i) {
                    int32_t a = pb_vs(&b), o = pb_vs(&b);
                    if (double_delta) { dlat += a; dlon += o; lat += dlat; lon += dlon; }
                    else { lat += a; lon += o; }
                }
                if (kept < MAPFILE_MAX_POINTS) {
                    mf->lat[kept] = lat;
                    mf->lon[kept] = lon;
                    kept++;
                }
            }
            if (b.err) return ESP_ERR_INVALID_SIZE;
            if (kept < n) mf->skipped++;   /* truncated block */
            if (k == 0 && d == 0 && w.has_label_pos) {
                w.label_lat = mf->lat[0] + label_dlat;
                w.label_lon = mf->lon[0] + label_dlon;
            }
            w.part = d;
            w.block = k;
            w.n_blocks = n_blocks;
            w.n_points = kept;
            w.lat = mf->lat;
            w.lon = mf->lon;
            cb(&w, ctx);
        }
    }
    return b.p == b.end ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t mapfile_read_base_tile(mapfile_t *mf, const mapfile_zoom_interval_t *z, uint32_t btx, uint32_t bty,
                                 uint8_t zoom, bool *water, mapfile_way_cb_t cb, void *ctx)
{
    if (water) *water = false;
    if (!mf->f) return ESP_ERR_INVALID_STATE;
    if (btx < z->tx0 || btx >= z->tx0 + z->tw || bty < z->ty0 || bty >= z->ty0 + z->th) {
        return ESP_ERR_NOT_FOUND;
    }
    if (zoom > z->max_zoom) zoom = z->max_zoom;
    uint32_t n_tiles = z->tw * z->th;
    uint32_t idx = (bty - z->ty0) * z->tw + (btx - z->tx0);
    uint64_t index_start = z->start + (mf->debug ? DEBUG_INDEX_SIG : 0);

    uint8_t e[2 * INDEX_ENTRY];
    size_t want = idx + 1 < n_tiles ? sizeof e : INDEX_ENTRY;
    if (fseek(mf->f, (long)(index_start + (uint64_t)idx * INDEX_ENTRY), SEEK_SET) != 0 ||
        fread(e, 1, want, mf->f) != want) {
        return ESP_FAIL;
    }
    uint64_t v = 0;
    for (int i = 0; i < INDEX_ENTRY; i++) v = (v << 8) | e[i];
    bool is_water = v & ((uint64_t)1 << 39);
    uint64_t off = v & (((uint64_t)1 << 39) - 1);
    uint64_t next;
    if (idx + 1 < n_tiles) {
        v = 0;
        for (int i = INDEX_ENTRY; i < 2 * INDEX_ENTRY; i++) v = (v << 8) | e[i];
        next = v & (((uint64_t)1 << 39) - 1);
    } else {
        next = z->size;
    }
    if (water) *water = is_water;
    if (next <= off) return ESP_OK;              /* empty tile */
    if (next > z->size) return ESP_ERR_INVALID_SIZE;

    rd_t r = { .f = mf->f, .left = next - off };
    if (fseek(mf->f, (long)(z->start + off), SEEK_SET) != 0) return ESP_FAIL;

    if (mf->debug) rd_bytes(&r, NULL, DEBUG_TILE_SIG);
    /* zoom table: POI and way counts per zoom level of the interval */
    uint32_t n_ways = 0;
    for (uint8_t lz = z->min_zoom; lz <= z->max_zoom; lz++) {
        uint32_t pois = rd_vu(&r);
        uint32_t ways = rd_vu(&r);
        (void)pois;
        if (lz <= zoom) n_ways += ways;
    }
    uint32_t first_way = rd_vu(&r);              /* skips the POIs */
    if (r.err || !rd_bytes(&r, NULL, first_way)) return ESP_ERR_INVALID_SIZE;

    int32_t olat = tile_origin_lat(bty, z->base_zoom);
    int32_t olon = tile_origin_lon(btx, z->base_zoom);

    for (uint32_t i = 0; i < n_ways; i++) {
        uint32_t len = rd_vu(&r);
        if (r.err || len > rd_remaining(&r)) return ESP_ERR_INVALID_SIZE;
        if (len > MAPFILE_WAY_BUF) {
            rd_bytes(&r, NULL, len);
            mf->skipped++;
            continue;
        }
        if (!rd_bytes(&r, mf->way_buf, len)) return ESP_ERR_INVALID_SIZE;
        esp_err_t err = parse_way(mf, mf->way_buf, len, olat, olon, cb, ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "base tile %lu/%lu z%u: way %lu of %lu corrupt", (unsigned long)btx,
                     (unsigned long)bty, z->base_zoom, (unsigned long)i, (unsigned long)n_ways);
            return err;
        }
        mf->ways++;
    }
    return ESP_OK;
}

esp_err_t mapfile_read_tile(mapfile_t *mf, uint8_t zoom, uint32_t tx, uint32_t ty,
                            bool *water, mapfile_way_cb_t cb, void *ctx)
{
    mf->ways = mf->skipped = 0;
    if (water) *water = false;
    if (!mf->f) return ESP_ERR_INVALID_STATE;
    const mapfile_zoom_interval_t *z = mapfile_interval(mf, zoom);
    if (!z) return ESP_ERR_NOT_FOUND;

    if (zoom >= z->base_zoom) {
        return mapfile_read_base_tile(mf, z, tx >> (zoom - z->base_zoom), ty >> (zoom - z->base_zoom),
                                      zoom, water, cb, ctx);
    }
    /* one tile at this zoom spans a square of base tiles */
    uint8_t diff = z->base_zoom - zoom;
    uint32_t n = 1u << diff;
    esp_err_t res = ESP_ERR_NOT_FOUND;
    bool all_water = true;
    for (uint32_t y = 0; y < n; y++) {
        for (uint32_t x = 0; x < n; x++) {
            bool w;
            esp_err_t err = mapfile_read_base_tile(mf, z, (tx << diff) + x, (ty << diff) + y, zoom, &w, cb, ctx);
            if (err == ESP_ERR_NOT_FOUND) continue;
            if (err != ESP_OK) return err;
            res = ESP_OK;
            all_water = all_water && w;
        }
    }
    if (water) *water = res == ESP_OK && all_water;
    return res;
}
