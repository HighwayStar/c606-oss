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
#include "config.h"
#include "route.h"
#include "trail.h"

static const char *TAG = "mapview";

#define MAP_DIR      SD_MOUNT_POINT "/MAP"
#define MAX_MAPS     4
#define ZOOM_MIN     10
#define ZOOM_MAX     17
#define ZOOM_DEFAULT 15
#define MOVE_PX      12          /* re-render once the position moved this far */
#define REFRESH_MS   500
#define TRAIL_REFRESH_MS 5000    /* new trail points alone re-render at most this often */

typedef struct {
    mapfile_t mf;
    bool gcj02;
    char name[48];
    uint8_t *tag_style;   /* per way tag id (n_way_tags): index into k_styles, NO_STYLE if none */
    uint32_t layers;      /* layer mask of the current render */
    uint8_t zoom;         /* display zoom of the current render */
} map_t;

/* Layer groups the user can switch off (bit i of app_cfg_t::map_layers). */
enum { L_MOTORWAY, L_PRIMARY, L_SECONDARY, L_TERTIARY, L_RESIDENTIAL, L_SERVICE, L_PEDESTRIAN,
       L_FOOTWAY, L_PATH, L_TRACK, L_CYCLEWAY, L_WATER, L_COASTLINE, L_OTHER,
       L_LANDUSE, L_GREEN, L_BUILDINGS, L_FILL, L_COUNT };   /* append only: bits are stored in the config */
static const char *k_layer_names[L_COUNT] = {
    "Motorway / trunk", "Primary", "Secondary", "Tertiary", "Residential", "Service", "Pedestrian",
    "Footway", "Path", "Track", "Cycleway", "Water", "Coastline", "Other",
    "Land use", "Parks / forest", "Buildings", "Fill areas",   /* off: areas as outlines */
};

/* Layers that are clutter at overview zooms (and costly to decode): only
 * drawn from this zoom on, 0 = always. */
static const uint8_t k_layer_min_zoom[L_COUNT] = {
    [L_FOOTWAY] = 14, [L_PATH] = 14, [L_BUILDINGS] = 15,
};

/* Drawing order: every pixel of the back buffer remembers the priority of
 * what was drawn there (s_prio) and only equal or higher priorities paint
 * over it, so the ways can be drawn in whatever order the file delivers
 * them. Areas lowest, then lines, major roads on top. */
enum { P_SEA = 1, P_NOSEA, P_LANDUSE, P_GREEN, P_WATER, P_BUILDING, P_BUILDING_OUTLINE,
       P_WATERWAY = 10, P_OTHER, P_MINOR, P_MAJOR, P_OVERLAY = 255 };

/* colour (light theme, dark theme), width, priority and layer group per way
 * tag; the first matching prefix wins. Area styles fill closed ways (open
 * ones are drawn as 1 px lines in the fill colour); `width` > 0 adds an
 * outline in (ol_light, ol_dark) from zoom 16 on. */
typedef struct {
    const char *tag;
    uint16_t light, dark; /* RGB565 */
    uint8_t width;
    uint8_t prio;
    uint8_t layer;
    bool area;
    uint16_t ol_light, ol_dark;
} style_t;

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define LINE(tag, l, d, w, prio, layer)       { tag, l, d, w, prio, layer, false, 0, 0 }
#define AREA(tag, l, d, prio, layer)          { tag, l, d, 0, prio, layer, true, 0, 0 }
#define AREA_OL(tag, l, d, ol, od, prio, layer) { tag, l, d, 1, prio, layer, true, ol, od }

#define C_WATER_L  RGB(0x9c, 0xc8, 0xdc)
#define C_WATER_D  RGB(0x24, 0x3c, 0x58)
#define C_GREEN_L  RGB(0xc4, 0xe4, 0xb0)
#define C_GREEN_D  RGB(0x1e, 0x30, 0x22)
#define C_WOOD_L   RGB(0xa8, 0xd0, 0x98)
#define C_WOOD_D   RGB(0x1a, 0x36, 0x20)
#define C_LAND_L   RGB(0xdc, 0xd8, 0xd0)   /* residential & co. */
#define C_LAND_D   RGB(0x24, 0x26, 0x2a)
#define C_WORK_L   RGB(0xe4, 0xd4, 0xd8)   /* industrial, commercial, retail */
#define C_WORK_D   RGB(0x2a, 0x24, 0x2a)

static const style_t k_styles[] = {
    LINE("highway=motorway",      RGB(0xe0, 0x80, 0x92), RGB(0xb0, 0x50, 0x60), 5, P_MAJOR, L_MOTORWAY),
    LINE("highway=trunk",         RGB(0xf4, 0xa0, 0x88), RGB(0xc0, 0x70, 0x58), 4, P_MAJOR, L_MOTORWAY),
    LINE("highway=primary",       RGB(0xf6, 0xc8, 0x8c), RGB(0xc8, 0x98, 0x60), 4, P_MAJOR, L_PRIMARY),
    LINE("highway=secondary",     RGB(0xf0, 0xf0, 0xa0), RGB(0xb0, 0xb0, 0x70), 3, P_MAJOR, L_SECONDARY),
    LINE("highway=tertiary",      RGB(0xff, 0xff, 0xff), RGB(0xa0, 0xa0, 0xa8), 3, P_MAJOR, L_TERTIARY),
    LINE("highway=residential",   RGB(0xff, 0xff, 0xff), RGB(0x80, 0x80, 0x88), 2, P_MINOR, L_RESIDENTIAL),
    LINE("highway=unclassified",  RGB(0xff, 0xff, 0xff), RGB(0x80, 0x80, 0x88), 2, P_MINOR, L_RESIDENTIAL),
    LINE("highway=living_street", RGB(0xf4, 0xf4, 0xf4), RGB(0x70, 0x70, 0x78), 2, P_MINOR, L_RESIDENTIAL),
    LINE("highway=pedestrian",    RGB(0xe0, 0xe0, 0xee), RGB(0x68, 0x68, 0x80), 2, P_MINOR, L_PEDESTRIAN),
    LINE("highway=service",       RGB(0xf0, 0xf0, 0xf0), RGB(0x60, 0x60, 0x68), 1, P_MINOR, L_SERVICE),
    LINE("highway=road",          RGB(0xf0, 0xf0, 0xf0), RGB(0x60, 0x60, 0x68), 1, P_MINOR, L_SERVICE),
    LINE("highway=cycleway",      RGB(0x40, 0x40, 0xff), RGB(0x60, 0x60, 0xe0), 1, P_MINOR, L_CYCLEWAY),
    LINE("highway=footway",       RGB(0xf0, 0x70, 0x60), RGB(0x98, 0x50, 0x48), 1, P_MINOR, L_FOOTWAY),
    LINE("highway=path",          RGB(0xa0, 0x60, 0x30), RGB(0x88, 0x60, 0x40), 1, P_MINOR, L_PATH),
    LINE("highway=track",         RGB(0x90, 0x60, 0x20), RGB(0x80, 0x60, 0x38), 1, P_MINOR, L_TRACK),
    LINE("natural=coastline",     RGB(0x30, 0x60, 0xa0), RGB(0x40, 0x70, 0xb0), 2, P_WATERWAY, L_COASTLINE),
    LINE("waterway=river",        C_WATER_L, C_WATER_D, 3, P_WATERWAY, L_WATER),
    LINE("waterway=canal",        C_WATER_L, C_WATER_D, 2, P_WATERWAY, L_WATER),
    LINE("waterway=stream",       C_WATER_L, C_WATER_D, 1, P_WATERWAY, L_WATER),
    LINE("waterway=drain",        C_WATER_L, C_WATER_D, 1, P_WATERWAY, L_WATER),
    LINE("waterway=ditch",        C_WATER_L, C_WATER_D, 1, P_WATERWAY, L_WATER),
    /* areas */
    AREA("natural=water",         C_WATER_L, C_WATER_D, P_WATER, L_WATER),
    AREA("waterway=riverbank",    C_WATER_L, C_WATER_D, P_WATER, L_WATER),
    AREA("landuse=reservoir",     C_WATER_L, C_WATER_D, P_WATER, L_WATER),
    AREA("landuse=basin",         C_WATER_L, C_WATER_D, P_WATER, L_WATER),
    AREA("natural=sea",           C_WATER_L, C_WATER_D, P_SEA, L_WATER),
    AREA("natural=nosea",         0, 0, P_NOSEA, L_WATER),   /* colour 0 = map background */
    AREA_OL("building=",          RGB(0xd4, 0xcc, 0xc4), RGB(0x38, 0x38, 0x40),
                                  RGB(0xb8, 0xac, 0xa0), RGB(0x50, 0x50, 0x5a), P_BUILDING, L_BUILDINGS),
    AREA("natural=wood",          C_WOOD_L, C_WOOD_D, P_GREEN, L_GREEN),
    AREA("landuse=forest",        C_WOOD_L, C_WOOD_D, P_GREEN, L_GREEN),
    AREA("leisure=park",          C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("leisure=garden",        C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("leisure=playground",    C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("leisure=pitch",         RGB(0xb0, 0xdc, 0xc4), RGB(0x1e, 0x36, 0x30), P_GREEN, L_GREEN),
    AREA("leisure=nature_reserve", 0, 0, 0, L_GREEN),   /* huge, would hide everything: not drawn */
    AREA("landuse=grass",         C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("landuse=meadow",        C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("landuse=recreation_ground", C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("landuse=village_green", C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("landuse=cemetery",      RGB(0xb0, 0xcc, 0xb0), RGB(0x22, 0x30, 0x26), P_GREEN, L_GREEN),
    AREA("landuse=allotments",    RGB(0xd4, 0xe0, 0xb8), RGB(0x26, 0x2e, 0x22), P_GREEN, L_GREEN),
    AREA("landuse=orchard",       RGB(0xc0, 0xe0, 0xa8), RGB(0x22, 0x32, 0x22), P_GREEN, L_GREEN),
    AREA("natural=grassland",     C_GREEN_L, C_GREEN_D, P_GREEN, L_GREEN),
    AREA("natural=scrub",         RGB(0xc4, 0xd8, 0xa8), RGB(0x22, 0x2e, 0x22), P_GREEN, L_GREEN),
    AREA("natural=heath",         RGB(0xd4, 0xd8, 0xa8), RGB(0x2a, 0x2e, 0x22), P_GREEN, L_GREEN),
    AREA("natural=wetland",       RGB(0xc4, 0xd8, 0xcc), RGB(0x20, 0x2e, 0x2e), P_GREEN, L_GREEN),
    AREA("landuse=farmland",      RGB(0xec, 0xe4, 0xc8), RGB(0x2a, 0x2a, 0x22), P_LANDUSE, L_LANDUSE),
    AREA("landuse=industrial",    C_WORK_L, C_WORK_D, P_LANDUSE, L_LANDUSE),
    AREA("landuse=commercial",    C_WORK_L, C_WORK_D, P_LANDUSE, L_LANDUSE),
    AREA("landuse=retail",        C_WORK_L, C_WORK_D, P_LANDUSE, L_LANDUSE),
    AREA("landuse=military",      RGB(0xe8, 0xcc, 0xc4), RGB(0x30, 0x24, 0x24), P_LANDUSE, L_LANDUSE),
    AREA("natural=beach",         RGB(0xf0, 0xe4, 0xb4), RGB(0x34, 0x30, 0x24), P_LANDUSE, L_LANDUSE),
    AREA("natural=sand",          RGB(0xf0, 0xe4, 0xb4), RGB(0x34, 0x30, 0x24), P_LANDUSE, L_LANDUSE),
    AREA("amenity=parking",       RGB(0xe8, 0xe8, 0xe8), RGB(0x30, 0x30, 0x34), P_LANDUSE, L_LANDUSE),
    AREA("amenity=school",        RGB(0xf0, 0xf0, 0xd8), RGB(0x2e, 0x2e, 0x26), P_LANDUSE, L_LANDUSE),
    AREA("amenity=kindergarten",  RGB(0xf0, 0xf0, 0xd8), RGB(0x2e, 0x2e, 0x26), P_LANDUSE, L_LANDUSE),
    AREA("amenity=university",    RGB(0xf0, 0xf0, 0xd8), RGB(0x2e, 0x2e, 0x26), P_LANDUSE, L_LANDUSE),
    AREA("amenity=hospital",      RGB(0xf0, 0xf0, 0xd8), RGB(0x2e, 0x2e, 0x26), P_LANDUSE, L_LANDUSE),
    AREA("landuse=",              C_LAND_L, C_LAND_D, P_LANDUSE, L_LANDUSE),   /* residential and the rest */
};
#define NO_STYLE 0xFF
static const style_t k_default_style = LINE("", RGB(0x90, 0x90, 0x90), RGB(0x70, 0x70, 0x70), 1, P_OTHER, L_OTHER);
static const uint16_t k_route_light = RGB(0xc0, 0x20, 0xa0), k_route_dark = RGB(0xe0, 0x50, 0xd0);
static const uint16_t k_trail_light = RGB(0x1e, 0x88, 0xe5), k_trail_dark = RGB(0x42, 0xa5, 0xf5);
static const uint16_t k_bg_light = RGB(0xe4, 0xe0, 0xd6);   /* a little darker than the page */
static const uint16_t k_bg_dark  = RGB(0x1c, 0x1e, 0x22);
static bool s_dark;                    /* palette of the current render */
static bool s_shown_dark;
static uint32_t s_shown_layers, s_shown_route, s_shown_trail;
static int64_t s_shown_at;             /* esp_timer time of the last render */

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
    bool filling;            /* the current polygon is being filled */
    int64_t t_draw;          /* us spent in the drawing callback */
    int64_t t_fill;          /* of which in fill_edges() */
} render_t;

static uint8_t *s_prio;      /* drawing priority per pixel of s_back (P_*) */

static inline void put_px(int x, int y, uint16_t c, uint8_t prio)
{
    if ((unsigned)x < (unsigned)s_w && (unsigned)y < (unsigned)s_h) {
        int i = y * s_w + x;
        if (s_prio[i] <= prio) {
            s_back[i] = c;
            s_prio[i] = prio;
        }
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t c, int width, uint8_t prio)
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
            if (steep) put_px(x0 + i, y0, c, prio); else put_px(x0, y0 + i, c, prio);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static const style_t *style_for_tag(const char *tag)
{
    for (size_t i = 0; i < sizeof k_styles / sizeof k_styles[0]; i++) {
        if (strncmp(tag, k_styles[i].tag, strlen(k_styles[i].tag)) == 0) return &k_styles[i];
    }
    return &k_default_style;
}

/* Style of a way: the first of its tags that has one (tags like oneway=yes
 * or building:levels come in any order). */
static const style_t *style_of_tags(const map_t *map, const uint16_t *tags, int ntags)
{
    if (!map->tag_style) return &k_default_style;
    for (int i = 0; i < ntags; i++) {
        if (tags[i] < map->mf.n_way_tags && map->tag_style[tags[i]] != NO_STYLE) {
            return &k_styles[map->tag_style[tags[i]]];
        }
    }
    return &k_default_style;
}

/* Per map: the style of every way tag id, looked up once so styling a way
 * during a render is an array read per tag. */
static void map_apply_layers(map_t *map, uint32_t layers)
{
    map->layers = layers;
    if (map->tag_style) return;
    uint16_t n = map->mf.n_way_tags;
    map->tag_style = malloc(n ? n : 1);
    if (!map->tag_style) {
        ESP_LOGW(TAG, "no memory for the style table, drawing everything plain");
        return;
    }
    for (int i = 0; i < n; i++) {
        const style_t *st = style_for_tag(map->mf.way_tags[i]);
        map->tag_style[i] = st == &k_default_style ? NO_STYLE : (uint8_t)(st - k_styles);
    }
}

/* mapfile filter: a way is drawn when the layer group of its style is
 * enabled; ways without a styled tag count as "Other" */
static bool way_filter(const uint16_t *tags, int ntags, void *ctx)
{
    const map_t *map = ctx;
    const style_t *st = style_of_tags(map, tags, ntags);
    if (map->zoom < k_layer_min_zoom[st->layer]) return false;
    return (map->layers & (1u << st->layer)) && st->prio;
}

int mapview_layer_count(void) { return L_COUNT; }
const char *mapview_layer_name(int i) { return i >= 0 && i < L_COUNT ? k_layer_names[i] : ""; }

static inline int16_t clamp16(int v)
{
    return v < -32000 ? -32000 : v > 32000 ? 32000 : v;
}

/* ---- polygon fill: scanline, even-odd rule (holes are just more edges) --- */

typedef struct {
    float x, dxdy;           /* x at the centre of scanline y0, step per line */
    int16_t y0, y1;          /* scanlines [y0, y1) */
} edge_t;

#define EDGE_MAX 8192
static edge_t *s_edges;
static uint16_t *s_active;
static float *s_xs;
static uint32_t s_n_edges;

static void add_edge(float xa, float ya, float xb, float yb)
{
    if (ya == yb) return;
    if (ya > yb) {
        float t = xa; xa = xb; xb = t;
        t = ya; ya = yb; yb = t;
    }
    /* entirely right of the screen: changes no span inside it */
    if (xa >= s_w && xb >= s_w) return;
    if (yb < 0 || ya > s_h) return;
    int y0 = (int)ceilf(ya - 0.5f), y1 = (int)ceilf(yb - 0.5f);
    if (y0 < 0) y0 = 0;
    if (y1 > s_h) y1 = s_h;
    if (y0 >= y1 || s_n_edges >= EDGE_MAX) return;
    edge_t *e = &s_edges[s_n_edges++];
    e->dxdy = (xb - xa) / (yb - ya);
    e->x = xa + ((float)y0 + 0.5f - ya) * e->dxdy;
    e->y0 = (int16_t)y0;
    e->y1 = (int16_t)y1;
}

static int edge_cmp(const void *a, const void *b)
{
    return ((const edge_t *)a)->y0 - ((const edge_t *)b)->y0;
}

/* Fills the polygon made of the collected edges and forgets them. */
static void fill_edges(uint16_t c, uint8_t prio)
{
    uint32_t n = s_n_edges;
    s_n_edges = 0;
    if (n < 2) return;
    qsort(s_edges, n, sizeof *s_edges, edge_cmp);
    uint32_t next = 0, n_act = 0;
    for (int y = s_edges[0].y0; y < s_h; y++) {
        while (next < n && s_edges[next].y0 == y) s_active[n_act++] = (uint16_t)next++;
        uint32_t k = 0, nx = 0;
        for (uint32_t i = 0; i < n_act; i++) {
            edge_t *e = &s_edges[s_active[i]];
            if (e->y1 <= y) continue;
            s_active[k++] = s_active[i];
            float x = e->x;
            e->x += e->dxdy;
            uint32_t j = nx++;   /* insertion sort, a row crosses few edges */
            while (j && s_xs[j - 1] > x) { s_xs[j] = s_xs[j - 1]; j--; }
            s_xs[j] = x;
        }
        n_act = k;
        uint16_t *row = s_back + y * s_w;
        uint8_t *prow = s_prio + y * s_w;
        for (uint32_t i = 0; i + 1 < nx; i += 2) {
            float fa = s_xs[i] - 0.5f, fb = s_xs[i + 1] - 0.5f;
            int xa = fa <= 0 ? 0 : (int)ceilf(fa);
            int xb = fb >= s_w ? s_w : (int)ceilf(fb);
            for (int x = xa; x < xb; x++) {
                if (prow[x] <= prio) {
                    row[x] = c;
                    prow[x] = prio;
                }
            }
        }
        if (!n_act && next >= n) break;
    }
}

static inline void project(const render_t *r, int32_t lat, int32_t lon, float *x, float *y)
{
    *x = r->ox + (float)(lon - r->olon) * r->sx;
    *y = r->oy - (float)(lat - r->olat) * r->sy;
}

static void way_cb(const mapfile_way_t *w, void *ctx)
{
    render_t *r = ctx;
    int64_t t0 = esp_timer_get_time();
    const style_t *st = style_of_tags(r->map, w->tags, w->tag_count);
    r->blocks++;
    r->points += w->n_points;
    uint16_t color = style_color(st);
    if (st->area && st->light == 0 && st->dark == 0) color = s_dark ? k_bg_dark : k_bg_light;
    /* areas: rings of one polygon arrive as consecutive blocks, the first
     * one decides whether it is closed (fill) or not (outline only) */
    if (w->block == 0) {
        r->filling = st->area && s_edges && (r->map->layers & (1u << L_FILL)) && w->n_points >= 3 &&
                     (w->n_blocks > 1 || (w->lat[0] == w->lat[w->n_points - 1] &&
                                          w->lon[0] == w->lon[w->n_points - 1]));
        s_n_edges = 0;
    }
    bool outline = !st->area || !r->filling || (st->width && r->zoom >= 16);
    uint16_t lc = st->area && (r->filling || st->width) ? (s_dark ? st->ol_dark : st->ol_light) : color;
    uint8_t lprio = st->area && r->filling ? st->prio + 1 : st->prio;
    int lw = st->area ? 1 : st->width;
    float fx0 = 0, fy0 = 0, fpx = 0, fpy = 0;
    int px = 0, py = 0;
    for (int i = 0; i < w->n_points; i++) {
        float fx, fy;
        project(r, w->lat[i], w->lon[i], &fx, &fy);
        int x = (int)lroundf(fx), y = (int)lroundf(fy);
        if (r->filling) {
            if (i) add_edge(fpx, fpy, fx, fy);
            else { fx0 = fx; fy0 = fy; }
        }
        if (outline && i) draw_line(clamp16(px), clamp16(py), clamp16(x), clamp16(y), lc, lw, lprio);
        fpx = fx;
        fpy = fy;
        px = x;
        py = y;
    }
    if (r->filling) {
        add_edge(fpx, fpy, fx0, fy0);   /* close the ring */
        if (w->block + 1 == w->n_blocks) {
            int64_t t1 = esp_timer_get_time();
            fill_edges(color, st->prio);
            r->t_fill += esp_timer_get_time() - t1;
        }
    }
    r->t_draw += esp_timer_get_time() - t0;
}

/* Renders one map around (lat, lon) into s_back. Returns the number of base tiles read. */
static int render_map(map_t *map, double lat, double lon, uint8_t zoom, uint32_t layers)
{
    map_apply_layers(map, layers);
    map->zoom = zoom;
    map->mf.filter = way_filter;
    map->mf.filter_ctx = map;
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
    map->mf.ways = map->mf.skipped = map->mf.filtered = 0;
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
    ESP_LOGD(TAG, "%s z%u: %d tiles, %lu ways, %lu pts: read %d ms, draw %d ms (fill %d)", map->name, zoom, tiles,
             (unsigned long)map->mf.ways, (unsigned long)r.points, (int)((t1 - t0 - r.t_draw) / 1000),
             (int)(r.t_draw / 1000), (int)(r.t_fill / 1000));
    return tiles;
}

static void scale_bar_update(double lat, uint8_t zoom);

/* The loaded GPX route on top of the map: points are zoom-20 pixels, the
 * screen is a window at `zoom` around (cx, cy) (zoom-20 pixels too).
 * `dx`, `dy` shift the route like the position when the map is GCJ-02. */
static void draw_route(double cx20, double cy20, uint8_t zoom, double dx, double dy)
{
    const int32_t *x, *y;
    route_lock();
    size_t n = route_points(&x, &y);
    float scale = ldexpf(1.0f, (int)zoom - 20);
    uint16_t c = s_dark ? k_route_dark : k_route_light;
    int px = 0, py = 0;
    for (size_t i = 0; i < n; i++) {
        int sx = (int)lroundf((float)((double)x[i] - cx20 + dx) * scale) + s_w / 2;
        int sy = (int)lroundf((float)((double)y[i] - cy20 + dy) * scale) + s_h / 2;
        if (i && (sx != px || sy != py || i == n - 1)) draw_line(px, py, sx, sy, c, 4, P_OVERLAY);
        px = sx;
        py = sy;
    }
    /* start (green) and end (red) squares */
    if (n) {
        for (int k = 0; k < 2; k++) {
            size_t i = k ? n - 1 : 0;
            int sx = (int)lroundf((float)((double)x[i] - cx20 + dx) * scale) + s_w / 2;
            int sy = (int)lroundf((float)((double)y[i] - cy20 + dy) * scale) + s_h / 2;
            uint16_t mc = k ? RGB(0xe0, 0x30, 0x30) : RGB(0x30, 0xb0, 0x40);
            for (int yy = -4; yy <= 4; yy++) for (int xx = -4; xx <= 4; xx++) put_px(sx + xx, sy + yy, mc, P_OVERLAY);
        }
    }
    route_unlock();
}

/* The path ridden so far (trail.h), same projection as the route; drawn
 * over the route so the covered part of it turns blue. */
static void draw_trail(double cx20, double cy20, uint8_t zoom, double dx, double dy)
{
    const int32_t *x, *y;
    trail_lock();
    size_t n = trail_points(&x, &y);
    float scale = ldexpf(1.0f, (int)zoom - 20);
    uint16_t c = s_dark ? k_trail_dark : k_trail_light;
    int px = 0, py = 0;
    for (size_t i = 0; i < n; i++) {
        int sx = (int)lroundf((float)((double)x[i] - cx20 + dx) * scale) + s_w / 2;
        int sy = (int)lroundf((float)((double)y[i] - cy20 + dy) * scale) + s_h / 2;
        if (i && (sx != px || sy != py || i == n - 1)) draw_line(px, py, sx, sy, c, 3, P_OVERLAY);
        px = sx;
        py = sy;
    }
    trail_unlock();
}

static void render_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
again:
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (!s_have_pos || !s_back || (!s_nmaps && !route_loaded())) { xSemaphoreGive(s_mtx); continue; }
        s_busy = true;
        double lat = s_lat, lon = s_lon;
        uint8_t zoom = s_zoom;
        int32_t w = s_w, h = s_h;
        s_dark = theme_current() == THEME_DARK;
        uint32_t layers = config_get()->map_layers;
        int64_t t0 = esp_timer_get_time();

        uint16_t bg = s_dark ? k_bg_dark : k_bg_light;
        for (int32_t i = 0; i < w * h; i++) s_back[i] = bg;
        memset(s_prio, 0, (size_t)w * h);
        int tiles = 0;
        uint32_t ways = 0;
        double gcj_dx = 0, gcj_dy = 0;   /* route shift on a GCJ-02 map */
        for (int i = 0; i < s_nmaps; i++) {
            tiles += render_map(&s_maps[i], lat, lon, zoom, layers);
            ways += s_maps[i].mf.ways - s_maps[i].mf.filtered;   /* drawn ways */
            if (s_maps[i].gcj02 && mapfile_contains(&s_maps[i].mf, (int32_t)(lat * 1e6), (int32_t)(lon * 1e6))) {
                double glat, glon;
                wgs_to_gcj(lat, lon, &glat, &glon);
                gcj_dx = mapfile_lon_to_px(glon, 20) - mapfile_lon_to_px(lon, 20);
                gcj_dy = mapfile_lat_to_py(glat, 20) - mapfile_lat_to_py(lat, 20);
            }
        }
        uint32_t route_gen = route_generation(), trail_gen = trail_generation();
        if (route_loaded()) draw_route(mapfile_lon_to_px(lon, 20), mapfile_lat_to_py(lat, 20), zoom, gcj_dx, gcj_dy);
        draw_trail(mapfile_lon_to_px(lon, 20), mapfile_lat_to_py(lat, 20), zoom, gcj_dx, gcj_dy);
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        xSemaphoreGive(s_mtx);

        ui_lock();
        if (s_front && w == s_w && h == s_h) {   /* the area may have been resized meanwhile */
            memcpy(s_front, s_back, (size_t)w * h * 2);
            s_shown_lat = lat;
            s_shown_lon = lon;
            s_shown_zoom = zoom;
            s_shown_dark = s_dark;
            s_shown_layers = layers;
            s_shown_route = route_gen;
            s_shown_trail = trail_gen;
            s_shown_at = esp_timer_get_time();
            s_shown = true;
            scale_bar_update(lat, zoom);
            snprintf(s_status, sizeof s_status, "z%u %lu w %d ms%s", zoom, (unsigned long)ways, ms,
                     tiles || !s_nmaps ? "" : " off map");
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
    bool need = s_have_pos && (!s_shown || s_shown_zoom != s_zoom || s_shown_dark != (theme_current() == THEME_DARK)
                               || s_shown_layers != config_get()->map_layers || s_shown_route != route_generation());
    if (s_have_pos && !need) {
        double dx = mapfile_lon_to_px(s_lon, s_zoom) - mapfile_lon_to_px(s_shown_lon, s_zoom);
        double dy = mapfile_lat_to_py(s_lat, s_zoom) - mapfile_lat_to_py(s_shown_lat, s_zoom);
        need = fabs(dx) > MOVE_PX || fabs(dy) > MOVE_PX;
    }
    /* the trail grew (or was reset) without the centre moving much: the
     * marker's tail catches up now and then rather than every second */
    if (s_have_pos && !need && s_shown_trail != trail_generation()) {
        need = esp_timer_get_time() - s_shown_at > (int64_t)TRAIL_REFRESH_MS * 1000;
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
        s_prio = heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
        s_edges = heap_caps_malloc(EDGE_MAX * sizeof *s_edges, MALLOC_CAP_SPIRAM);
        s_active = heap_caps_malloc(EDGE_MAX * sizeof *s_active, MALLOC_CAP_SPIRAM);
        s_xs = heap_caps_malloc(EDGE_MAX * sizeof *s_xs, MALLOC_CAP_SPIRAM);
        if (!s_edges || !s_active || !s_xs) {
            ESP_LOGW(TAG, "no PSRAM for area filling, outlines only");
            free(s_edges);
            s_edges = NULL;
        }
    }
    if (!s_front || !s_back || !s_prio) {
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
    lv_label_set_text(s_nomap, s_nmaps || route_loaded() ? "Waiting for a GPS fix" : "No maps in " MAP_DIR);

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
    for (int i = 0; i < s_nmaps; i++) {
        mapfile_close(&s_maps[i].mf);
        free(s_maps[i].tag_style);
        s_maps[i].tag_style = NULL;
    }
    s_nmaps = 0;
    xSemaphoreGive(s_mtx);
}

bool mapview_available(void) { return s_nmaps > 0 || route_loaded(); }
int mapview_map_count(void) { return s_nmaps; }
