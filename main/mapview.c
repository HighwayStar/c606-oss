/* Map page, see mapview.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "sdcard.h"
#include "mapfile.h"
#include "mapview.h"
#include "ui_port.h"
#include "theme.h"

static const char *TAG = "mapview";

#define MAP_DIR      SD_MOUNT_POINT "/MAP"
#define MAX_MAPS     4
#define ZOOM_MIN     10
#define ZOOM_MAX     17
#define ZOOM_DEFAULT 15
#define MOVE_PX      12          /* re-render once the position moved this far */
#define REFRESH_MS   500

typedef struct {
    mapfile_t mf;
    bool gcj02;
    char name[48];
} map_t;

/* colour (light theme, dark theme) and width per way tag; the first
 * matching prefix wins */
typedef struct {
    const char *tag;
    uint16_t light, dark; /* RGB565 */
    uint8_t width;
    uint8_t pass;         /* 0 = drawn first (areas, minor roads), 1 = on top */
} style_t;

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const style_t k_styles[] = {
    { "highway=motorway",      RGB(0xe0, 0x80, 0x92), RGB(0xb0, 0x50, 0x60), 5, 1 },
    { "highway=trunk",         RGB(0xf4, 0xa0, 0x88), RGB(0xc0, 0x70, 0x58), 4, 1 },
    { "highway=primary",       RGB(0xf6, 0xc8, 0x8c), RGB(0xc8, 0x98, 0x60), 4, 1 },
    { "highway=secondary",     RGB(0xf0, 0xf0, 0xa0), RGB(0xb0, 0xb0, 0x70), 3, 1 },
    { "highway=tertiary",      RGB(0xff, 0xff, 0xff), RGB(0xa0, 0xa0, 0xa8), 3, 1 },
    { "highway=residential",   RGB(0xff, 0xff, 0xff), RGB(0x80, 0x80, 0x88), 2, 0 },
    { "highway=unclassified",  RGB(0xff, 0xff, 0xff), RGB(0x80, 0x80, 0x88), 2, 0 },
    { "highway=living_street", RGB(0xf4, 0xf4, 0xf4), RGB(0x70, 0x70, 0x78), 2, 0 },
    { "highway=pedestrian",    RGB(0xe0, 0xe0, 0xee), RGB(0x68, 0x68, 0x80), 2, 0 },
    { "highway=service",       RGB(0xf0, 0xf0, 0xf0), RGB(0x60, 0x60, 0x68), 1, 0 },
    { "highway=road",          RGB(0xf0, 0xf0, 0xf0), RGB(0x60, 0x60, 0x68), 1, 0 },
    { "highway=cycleway",      RGB(0x40, 0x40, 0xff), RGB(0x60, 0x60, 0xe0), 1, 0 },
    { "highway=footway",       RGB(0xf0, 0x70, 0x60), RGB(0x98, 0x50, 0x48), 1, 0 },
    { "highway=path",          RGB(0xa0, 0x60, 0x30), RGB(0x88, 0x60, 0x40), 1, 0 },
    { "highway=track",         RGB(0x90, 0x60, 0x20), RGB(0x80, 0x60, 0x38), 1, 0 },
    { "natural=water",         RGB(0x90, 0xc0, 0xd8), RGB(0x30, 0x50, 0x70), 2, 0 },
    { "natural=coastline",     RGB(0x30, 0x60, 0xa0), RGB(0x40, 0x70, 0xb0), 2, 0 },
};
static const style_t k_default_style = { "", RGB(0x90, 0x90, 0x90), RGB(0x70, 0x70, 0x70), 1, 0 };
static const uint16_t k_bg_light = RGB(0xe4, 0xe0, 0xd6);   /* a little darker than the page */
static const uint16_t k_bg_dark  = RGB(0x1c, 0x1e, 0x22);
static bool s_dark;                    /* palette of the current render */
static bool s_shown_dark;

static inline uint16_t style_color(const style_t *st) { return s_dark ? st->dark : st->light; }

static map_t s_maps[MAX_MAPS];
static int s_nmaps;

static uint16_t *s_front, *s_back;    /* canvas buffer, render target (PSRAM) */
static int32_t s_w, s_h, s_max_h;
static lv_obj_t *s_canvas, *s_marker, *s_nomap;
static lv_timer_t *s_timer;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_mtx;       /* maps open/close vs. render, position updates */
static bool s_visible;

/* shared with the render task (under s_mtx): set by the UI, read at the start of a render */
static double s_lat, s_lon;
static bool s_have_pos, s_override;
static volatile uint8_t s_zoom = ZOOM_DEFAULT;
static volatile bool s_busy;          /* render in progress (refresh_cb waits for it) */
static double s_gps_lat, s_gps_lon;   /* last GPS fix, used when the override is removed */

/* what the front buffer shows */
static double s_shown_lat, s_shown_lon;
static uint8_t s_shown_zoom;
static bool s_shown;
static char s_status[64] = "no position";

/* ---- WGS-84 -> GCJ-02 (the usual "eviltransform") ------------------------ */

static double gcj_tf_lat(double x, double y)
{
    double r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * sqrt(fabs(x));
    r += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * sin(y * M_PI) + 40.0 * sin(y / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (160.0 * sin(y / 12.0 * M_PI) + 320.0 * sin(y * M_PI / 30.0)) * 2.0 / 3.0;
    return r;
}

static double gcj_tf_lon(double x, double y)
{
    double r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * sqrt(fabs(x));
    r += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * sin(x * M_PI) + 40.0 * sin(x / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (150.0 * sin(x / 12.0 * M_PI) + 300.0 * sin(x / 30.0 * M_PI)) * 2.0 / 3.0;
    return r;
}

static void wgs_to_gcj(double lat, double lon, double *olat, double *olon)
{
    const double a = 6378245.0, ee = 0.00669342162296594323;
    double dlat = gcj_tf_lat(lon - 105.0, lat - 35.0);
    double dlon = gcj_tf_lon(lon - 105.0, lat - 35.0);
    double rlat = lat / 180.0 * M_PI;
    double magic = sin(rlat);
    magic = 1 - ee * magic * magic;
    double sq = sqrt(magic);
    dlat = (dlat * 180.0) / ((a * (1 - ee)) / (magic * sq) * M_PI);
    dlon = (dlon * 180.0) / (a / sq * cos(rlat) * M_PI);
    *olat = lat + dlat;
    *olon = lon + dlon;
}

/* ---- rasteriser (into s_back) -------------------------------------------- */

typedef struct {
    map_t *map;
    uint8_t zoom;            /* display zoom */
    uint8_t data_zoom;       /* zoom the ways are read for */
    double cx, cy;           /* global pixel coordinates (display zoom) of the screen centre */
    /* per base tile: linearised projection around the tile origin */
    float ox, oy;            /* screen coordinates of the tile origin */
    float sx, sy;            /* pixels per microdegree */
    int32_t olat, olon;
    uint32_t blocks, points;
    int64_t t_draw;          /* us spent in the drawing callback */
} render_t;

/* Ways of pass 1 (major roads) are collected while the tile is read and
 * drawn after everything else so they end up on top: a flat list of
 * [style index, n, x0, y0, x1, y1, ...] in screen coordinates. */
#define DEFER_MAX 24576       /* int16 slots, 48 KB */
static int16_t *s_defer;
static uint32_t s_defer_n;

static inline void put_px(int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)s_w && (unsigned)y < (unsigned)s_h) s_back[y * s_w + x] = c;
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t c, int width)
{
    /* trivial reject: both ends beyond the same edge */
    int m = width;
    if ((x0 < -m && x1 < -m) || (x0 >= s_w + m && x1 >= s_w + m) ||
        (y0 < -m && y1 < -m) || (y0 >= s_h + m && y1 >= s_h + m)) {
        return;
    }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int half = width / 2;
    bool steep = dx <= -dy;
    for (;;) {
        for (int i = -half; i <= width - 1 - half; i++) {
            if (steep) put_px(x0 + i, y0, c); else put_px(x0, y0 + i, c);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static const style_t *style_for(const map_t *map, const mapfile_way_t *w)
{
    for (int t = 0; t < w->tag_count; t++) {
        const char *tag = map->mf.way_tags[w->tags[t]];
        for (size_t i = 0; i < sizeof k_styles / sizeof k_styles[0]; i++) {
            if (strncmp(tag, k_styles[i].tag, strlen(k_styles[i].tag)) == 0) return &k_styles[i];
        }
    }
    return &k_default_style;
}

static inline int16_t clamp16(int v)
{
    return v < -32000 ? -32000 : v > 32000 ? 32000 : v;
}

static void way_cb(const mapfile_way_t *w, void *ctx)
{
    render_t *r = ctx;
    int64_t t0 = esp_timer_get_time();
    const style_t *st = style_for(r->map, w);
    r->blocks++;
    r->points += w->n_points;
    bool defer = st->pass == 1 && s_defer && s_defer_n + 2 + 2u * w->n_points <= DEFER_MAX;
    if (defer) {
        s_defer[s_defer_n++] = (int16_t)(st - k_styles);
        s_defer[s_defer_n++] = w->n_points;
    }
    int px = 0, py = 0;
    for (int i = 0; i < w->n_points; i++) {
        int x = (int)lroundf(r->ox + (float)(w->lon[i] - r->olon) * r->sx);
        int y = (int)lroundf(r->oy - (float)(w->lat[i] - r->olat) * r->sy);
        if (defer) {
            s_defer[s_defer_n++] = clamp16(x);
            s_defer[s_defer_n++] = clamp16(y);
        } else if (i) {
            draw_line(px, py, x, y, style_color(st), st->width);
        }
        px = x;
        py = y;
    }
    r->t_draw += esp_timer_get_time() - t0;
}

static void draw_deferred(void)
{
    uint32_t i = 0;
    while (i + 2 <= s_defer_n) {
        const style_t *st = &k_styles[s_defer[i]];
        int n = s_defer[i + 1];
        i += 2;
        for (int k = 1; k < n; k++) {
            draw_line(s_defer[i + 2 * k - 2], s_defer[i + 2 * k - 1], s_defer[i + 2 * k], s_defer[i + 2 * k + 1],
                      style_color(st), st->width);
        }
        i += 2 * n;
    }
    s_defer_n = 0;
}

/* Renders one map around (lat, lon) into s_back. Returns the number of base tiles read. */
static int render_map(map_t *map, double lat, double lon, uint8_t zoom)
{
    if (map->gcj02) wgs_to_gcj(lat, lon, &lat, &lon);
    if (!mapfile_contains(&map->mf, (int32_t)(lat * 1e6), (int32_t)(lon * 1e6))) return 0;

    render_t r = { .map = map, .zoom = zoom };
    const mapfile_zoom_interval_t *z = NULL;
    for (uint8_t dz = zoom; dz > 0 && !z; dz--) {
        z = mapfile_interval(&map->mf, dz);
        if (z) r.data_zoom = dz;
    }
    if (!z) return 0;

    r.cx = mapfile_lon_to_px(lon, zoom);
    r.cy = mapfile_lat_to_py(lat, zoom);
    /* base tiles covering the screen */
    double f = ldexp(1.0, (int)z->base_zoom - (int)zoom) / 256.0;   /* display px -> base tile */
    double tx0 = floor((r.cx - s_w / 2.0) * f), tx1 = floor((r.cx + s_w / 2.0) * f);
    double ty0 = floor((r.cy - s_h / 2.0) * f), ty1 = floor((r.cy + s_h / 2.0) * f);
    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    int tiles = 0;
    map->mf.ways = map->mf.skipped = 0;
    int64_t t0 = esp_timer_get_time();

    for (double ty = ty0; ty <= ty1; ty++) {
        for (double tx = tx0; tx <= tx1; tx++) {
            uint32_t btx = (uint32_t)tx, bty = (uint32_t)ty;
            if (btx < z->tx0 || btx >= z->tx0 + z->tw || bty < z->ty0 || bty >= z->ty0 + z->th) continue;
            /* tile origin in microdegrees exactly as the file encodes it, and
             * the projection linearised around it (the lat scale is taken at
             * the tile's middle so the error stays below a pixel) */
            double olat_deg = mapfile_py_to_lat(bty * 256.0, z->base_zoom);
            double olon_deg = mapfile_px_to_lon(btx * 256.0, z->base_zoom);
            double mid_lat = mapfile_py_to_lat((bty + 0.5) * 256.0, z->base_zoom);
            r.olat = (int32_t)(olat_deg * 1e6);
            r.olon = (int32_t)(olon_deg * 1e6);
            r.ox = (float)(mapfile_lon_to_px(r.olon / 1e6, zoom) - r.cx + s_w / 2.0);
            r.oy = (float)(mapfile_lat_to_py(r.olat / 1e6, zoom) - r.cy + s_h / 2.0);
            double map_px = 256.0 * ldexp(1.0, zoom);
            r.sx = (float)(map_px / 360e6);
            r.sy = (float)(map_px / 360e6 / cos(mid_lat * M_PI / 180.0));
            esp_err_t err = mapfile_read_base_tile(&map->mf, z, btx, bty, r.data_zoom, NULL, way_cb, &r);
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "%s tile %lu/%lu: %s", map->name, (unsigned long)btx, (unsigned long)bty,
                         esp_err_to_name(err));
            }
            tiles++;
        }
    }
    int64_t t1 = esp_timer_get_time();
    draw_deferred();
    int64_t t2 = esp_timer_get_time();
    ESP_LOGD(TAG, "%s z%u: %d tiles, %lu ways, %lu pts: read %d ms, draw %d + %d ms", map->name, zoom, tiles,
             (unsigned long)map->mf.ways, (unsigned long)r.points, (int)((t1 - t0 - r.t_draw) / 1000),
             (int)(r.t_draw / 1000), (int)((t2 - t1) / 1000));
    return tiles;
}

static void scale_bar_update(double lat, uint8_t zoom);

static void render_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
again:
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (!s_have_pos || !s_back || !s_nmaps) { xSemaphoreGive(s_mtx); continue; }
        s_busy = true;
        double lat = s_lat, lon = s_lon;
        uint8_t zoom = s_zoom;
        int32_t w = s_w, h = s_h;
        s_dark = theme_current() == THEME_DARK;
        int64_t t0 = esp_timer_get_time();

        uint16_t bg = s_dark ? k_bg_dark : k_bg_light;
        for (int32_t i = 0; i < w * h; i++) s_back[i] = bg;
        int tiles = 0;
        uint32_t ways = 0;
        for (int i = 0; i < s_nmaps; i++) {
            tiles += render_map(&s_maps[i], lat, lon, zoom);
            ways += s_maps[i].mf.ways;
        }
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        xSemaphoreGive(s_mtx);

        ui_lock();
        if (s_front && w == s_w && h == s_h) {   /* the area may have been resized meanwhile */
            memcpy(s_front, s_back, (size_t)w * h * 2);
            s_shown_lat = lat;
            s_shown_lon = lon;
            s_shown_zoom = zoom;
            s_shown_dark = s_dark;
            s_shown = true;
            scale_bar_update(lat, zoom);
            snprintf(s_status, sizeof s_status, "z%u %lu w %d ms%s", zoom, (unsigned long)ways, ms,
                     tiles ? "" : " off map");
            if (s_canvas) lv_obj_invalidate(s_canvas);
            if (s_nomap) lv_obj_set_hidden(s_nomap, tiles > 0);
        }
        ui_unlock();
        s_busy = false;
        if (s_zoom != zoom || w != s_w || h != s_h) goto again;   /* zoomed / resized meanwhile */
    }
}

/* ---- UI side ------------------------------------------------------------- */

/* Re-render when the centre drifted or the zoom changed (LVGL task). */
static void refresh_cb(lv_timer_t *t)
{
    if (!s_visible || !s_task || s_busy) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool need = s_have_pos && (!s_shown || s_shown_zoom != s_zoom || s_shown_dark != (theme_current() == THEME_DARK));
    if (s_have_pos && !need) {
        double dx = mapfile_lon_to_px(s_lon, s_zoom) - mapfile_lon_to_px(s_shown_lon, s_zoom);
        double dy = mapfile_lat_to_py(s_lat, s_zoom) - mapfile_lat_to_py(s_shown_lat, s_zoom);
        need = fabs(dx) > MOVE_PX || fabs(dy) > MOVE_PX;
    }
    xSemaphoreGive(s_mtx);
    if (need) xTaskNotifyGive(s_task);
}

static lv_obj_t *s_scale_line, *s_scale_lbl;
static lv_point_precise_t s_scale_pts[2];
static int32_t s_y;                     /* top of the map area inside the page */

/* Scale bar: the longest of 20 m .. 50 km that fits in ~80 px (LVGL task). */
static void scale_bar_update(double lat, uint8_t zoom)
{
    if (!s_scale_line) return;
    /* metres per pixel at this latitude */
    double mpp = 40075016.686 * cos(lat * M_PI / 180.0) / (256.0 * ldexp(1.0, zoom));
    static const int k_steps[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000 };
    int m = k_steps[0];
    for (size_t i = 0; i < sizeof k_steps / sizeof k_steps[0]; i++) {
        if (k_steps[i] / mpp <= 80) m = k_steps[i];
    }
    int px = (int)(m / mpp);
    s_scale_pts[1].x = px;
    lv_line_set_points(s_scale_line, s_scale_pts, 2);
    if (m >= 1000) lv_label_set_text_fmt(s_scale_lbl, "%d km", m / 1000);
    else lv_label_set_text_fmt(s_scale_lbl, "%d m", m);
}

static void zoom_in_cb(lv_event_t *e) { mapview_zoom_by(1); }
static void zoom_out_cb(lv_event_t *e) { mapview_zoom_by(-1); }

static lv_obj_t *zoom_button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, int32_t x, int32_t y)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 40, 40);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *s_btn_in, *s_btn_out;

/* Places the widgets for a map area of w x h at (0, y) and brings them to
 * the front (the datapage strip may have been rebuilt underneath). */
static void place_widgets(void)
{
    lv_obj_set_pos(s_marker, s_w / 2 - 7, s_y + s_h / 2 - 7);
    lv_obj_align(s_nomap, LV_ALIGN_TOP_MID, 0, s_y + 12);
    lv_obj_set_pos(s_btn_in, s_w - 48, s_y + s_h - 96);
    lv_obj_set_pos(s_btn_out, s_w - 48, s_y + s_h - 48);
    lv_obj_set_pos(s_scale_line, 8, s_y + s_h - 10);
    lv_obj_align_to(s_scale_lbl, s_scale_line, LV_ALIGN_OUT_TOP_LEFT, 0, -2);
    lv_obj_t *objs[] = { s_canvas, s_marker, s_nomap, s_btn_in, s_btn_out, s_scale_line, s_scale_lbl };
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++) lv_obj_move_foreground(objs[i]);
}

lv_obj_t *mapview_create(lv_obj_t *parent, int32_t y, int32_t w, int32_t h)
{
    s_w = w;
    s_h = h;
    s_y = y;
    if (!s_front) {
        /* sized for the whole area below the header; mapview_set_area() only shrinks */
        s_front = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        s_back = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        s_defer = heap_caps_malloc(DEFER_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_front || !s_back) {
        ESP_LOGE(TAG, "no PSRAM for the map buffers");
        return NULL;
    }
    s_max_h = h;
    uint16_t bg = theme_current() == THEME_DARK ? k_bg_dark : k_bg_light;
    for (int32_t i = 0; i < w * h; i++) s_front[i] = bg;

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_front, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, y);

    /* position marker in the centre */
    s_marker = lv_obj_create(parent);
    lv_obj_remove_style_all(s_marker);
    lv_obj_set_size(s_marker, 14, 14);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_marker, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_marker, 3, 0);
    lv_obj_set_style_border_color(s_marker, lv_color_white(), 0);
    lv_obj_set_clickable(s_marker, false);

    s_nomap = lv_label_create(parent);
    lv_obj_set_style_text_font(s_nomap, &lv_font_montserrat_14, 0);
    lv_obj_add_style(s_nomap, &theme_st_muted, 0);
    lv_label_set_text(s_nomap, s_nmaps ? "Waiting for a GPS fix" : "No maps in " MAP_DIR);

    s_btn_in = zoom_button(parent, LV_SYMBOL_PLUS, zoom_in_cb, 0, 0);
    s_btn_out = zoom_button(parent, LV_SYMBOL_MINUS, zoom_out_cb, 0, 0);

    /* scale bar, bottom left */
    s_scale_pts[0].x = 0; s_scale_pts[0].y = 0;
    s_scale_pts[1].x = 60; s_scale_pts[1].y = 0;
    s_scale_line = lv_line_create(parent);
    lv_line_set_points(s_scale_line, s_scale_pts, 2);
    lv_obj_set_style_line_width(s_scale_line, 3, 0);
    lv_obj_set_style_line_color(s_scale_line, lv_color_make(0x30, 0x30, 0x30), 0);
    lv_obj_set_style_line_color(s_scale_line, theme_colors()->fg, 0);
    lv_obj_set_clickable(s_scale_line, false);
    s_scale_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(s_scale_lbl, &lv_font_montserrat_14, 0);
    lv_obj_add_style(s_scale_lbl, &theme_st_text, 0);
    lv_label_set_text(s_scale_lbl, "");
    place_widgets();

    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_task) {
        xTaskCreate(render_task, "mapview", 8192, NULL, 2, &s_task);
    }
    if (!s_timer) s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    return s_canvas;
}

void mapview_set_area(int32_t y, int32_t h)
{
    if (!s_canvas) return;
    if (h > s_max_h) h = s_max_h;
    if (h < 40) h = 40;
    s_y = y;
    s_h = h;
    uint16_t bg = theme_current() == THEME_DARK ? k_bg_dark : k_bg_light;
    for (int32_t i = 0; i < s_w * h; i++) s_front[i] = bg;
    lv_canvas_set_buffer(s_canvas, s_front, s_w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, y);
    lv_obj_set_style_line_color(s_scale_line, theme_colors()->fg, 0);
    place_widgets();
    s_shown = false;
    if (s_visible && s_task) xTaskNotifyGive(s_task);
}

void mapview_set_visible(bool visible)
{
    s_visible = visible;
    if (visible && s_task) xTaskNotifyGive(s_task);
}

void mapview_zoom_by(int delta)
{
    int z = (int)s_zoom + delta;
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    s_zoom = z;
    if (s_visible && s_task) xTaskNotifyGive(s_task);
}

uint8_t mapview_zoom(void) { return s_zoom; }

void mapview_set_position(double lat, double lon, bool valid)
{
    if (!valid || !s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_gps_lat = lat;
    s_gps_lon = lon;
    if (!s_override) {
        s_lat = lat;
        s_lon = lon;
        s_have_pos = true;
    }
    xSemaphoreGive(s_mtx);
}

void mapview_set_override(double lat, double lon, bool on)
{
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_override = on;
    if (on) {
        s_lat = lat;
        s_lon = lon;
        s_have_pos = true;
    } else if (s_have_pos) {
        s_lat = s_gps_lat;
        s_lon = s_gps_lon;
    }
    s_shown = false;   /* force a render */
    xSemaphoreGive(s_mtx);
    if (s_visible && s_task) xTaskNotifyGive(s_task);
}

const char *mapview_status(void) { return s_status; }

/* ---- maps ------------------------------------------------------------------ */

esp_err_t mapview_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    DIR *d = opendir(MAP_DIR);
    if (!d) {
        ESP_LOGW(TAG, "no " MAP_DIR);
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *e;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    while ((e = readdir(d)) != NULL && s_nmaps < MAX_MAPS) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".map") != 0) continue;
        char path[sizeof MAP_DIR + sizeof e->d_name + 1];
        snprintf(path, sizeof path, MAP_DIR "/%s", e->d_name);
        map_t *m = &s_maps[s_nmaps];
        if (mapfile_open(&m->mf, path) != ESP_OK) continue;
        strncpy(m->name, e->d_name, sizeof m->name - 1);
        m->gcj02 = strcasestr(e->d_name, "china") != NULL;
        ESP_LOGI(TAG, "map %d: %s%s", s_nmaps, m->name, m->gcj02 ? " (GCJ-02)" : "");
        s_nmaps++;
    }
    xSemaphoreGive(s_mtx);
    closedir(d);
    if (s_nomap && s_nmaps) {
        ui_lock();
        lv_label_set_text(s_nomap, "Waiting for a GPS fix");
        ui_unlock();
    }
    return s_nmaps ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void mapview_close(void)
{
    s_visible = false;
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);   /* waits for a running render */
    for (int i = 0; i < s_nmaps; i++) mapfile_close(&s_maps[i].mf);
    s_nmaps = 0;
    xSemaphoreGive(s_mtx);
}

bool mapview_available(void) { return s_nmaps > 0; }
int mapview_map_count(void) { return s_nmaps; }
