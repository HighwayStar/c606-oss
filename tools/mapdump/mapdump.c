/* Host tool for main/mapfile.c: dumps or renders one tile of a vendor map.
 *
 *   mapdump <file.map>                          header, tag table, zoom intervals
 *   mapdump <file.map> <lat> <lon> <zoom>       ways of the tile at that position (text)
 *   mapdump <file.map> <lat> <lon> <zoom> <out.ppm> [w h]   render the ways around the position
 *
 * Build: tools/mapdump/build.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mapfile.h"

typedef struct {
    mapfile_t *mf;
    /* render */
    uint8_t *img;
    int w, h;
    double cx, cy;   /* global pixel coordinates of the image centre */
    uint8_t zoom;
    int tiles, ways, points;
} ctx_t;

static void dump_cb(const mapfile_way_t *w, void *arg)
{
    ctx_t *c = arg;
    printf("way tags=[");
    for (int i = 0; i < w->tag_count; i++) printf("%s%s", i ? "," : "", c->mf->way_tags[w->tags[i]]);
    printf("] layer=%d name=%s ref=%s block=%u/%u n=%u", w->layer, w->name ? w->name : "-",
           w->ref ? w->ref : "-", w->block, w->n_blocks, w->n_points);
    for (int i = 0; i < w->n_points; i++) printf(" %ld,%ld", (long)w->lat[i], (long)w->lon[i]);
    printf("\n");
}

static void put(ctx_t *c, int x, int y, const uint8_t rgb[3])
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    memcpy(c->img + 3 * (y * c->w + x), rgb, 3);
}

static void line(ctx_t *c, int x0, int y0, int x1, int y1, const uint8_t rgb[3], int width)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int i = -(width / 2); i <= width / 2; i++) {
            if (dx > -dy) put(c, x0, y0 + i, rgb); else put(c, x0 + i, y0, rgb);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void style(const char *tag, uint8_t rgb[3], int *width)
{
    struct { const char *tag; uint8_t rgb[3]; int w; } tab[] = {
        { "highway=motorway", { 0xe8, 0x92, 0xa2 }, 5 }, { "highway=trunk", { 0xf9, 0xb2, 0x9c }, 4 },
        { "highway=primary", { 0xfc, 0xd6, 0xa4 }, 4 }, { "highway=secondary", { 0xf7, 0xfa, 0xbf }, 3 },
        { "highway=tertiary", { 0xff, 0xff, 0xff }, 3 }, { "highway=residential", { 0xff, 0xff, 0xff }, 2 },
        { "highway=unclassified", { 0xff, 0xff, 0xff }, 2 }, { "highway=service", { 0xdd, 0xdd, 0xdd }, 1 },
        { "highway=living_street", { 0xed, 0xed, 0xed }, 2 }, { "highway=pedestrian", { 0xdd, 0xdd, 0xe8 }, 2 },
        { "highway=footway", { 0xfa, 0x80, 0x72 }, 1 }, { "highway=path", { 0xaa, 0x66, 0x33 }, 1 },
        { "highway=track", { 0x99, 0x66, 0x22 }, 1 }, { "highway=cycleway", { 0x00, 0x00, 0xff }, 1 },
        { "natural=water", { 0xaa, 0xd3, 0xdf }, 2 }, { "natural=coastline", { 0x33, 0x66, 0xaa }, 2 },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strncmp(tag, tab[i].tag, strlen(tab[i].tag)) == 0) { memcpy(rgb, tab[i].rgb, 3); *width = tab[i].w; return; }
    }
    rgb[0] = rgb[1] = rgb[2] = 0x90; *width = 1;
}

static void render_cb(const mapfile_way_t *w, void *arg)
{
    ctx_t *c = arg;
    uint8_t rgb[3]; int width;
    style(w->tag_count ? c->mf->way_tags[w->tags[0]] : "", rgb, &width);
    c->ways++;
    c->points += w->n_points;
    int px = 0, py = 0;
    for (int i = 0; i < w->n_points; i++) {
        int x = (int)lround(mapfile_lon_to_px(w->lon[i] / 1e6, c->zoom) - c->cx) + c->w / 2;
        int y = (int)lround(mapfile_lat_to_py(w->lat[i] / 1e6, c->zoom) - c->cy) + c->h / 2;
        if (i) line(c, px, py, x, y, rgb, width);
        px = x; py = y;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mapdump file.map [lat lon zoom [out.ppm [w h]]]\n"); return 2; }
    mapfile_t mf;
    if (mapfile_open(&mf, argv[1]) != ESP_OK) return 1;
    if (argc < 5) {
        printf("bbox %ld,%ld .. %ld,%ld  tile size %u  debug %d\n", (long)mf.min_lat, (long)mf.min_lon,
               (long)mf.max_lat, (long)mf.max_lon, mf.tile_size, mf.debug);
        for (int i = 0; i < mf.n_way_tags; i++) printf("way tag %d: %s\n", i, mf.way_tags[i]);
        for (int i = 0; i < mf.n_zoom; i++) {
            const mapfile_zoom_interval_t *z = &mf.zoom[i];
            printf("interval %d: base %u zoom %u..%u start %llu size %llu tiles %lu..%lu x %lu..%lu\n", i,
                   z->base_zoom, z->min_zoom, z->max_zoom, (unsigned long long)z->start,
                   (unsigned long long)z->size, (unsigned long)z->tx0, (unsigned long)(z->tx0 + z->tw - 1),
                   (unsigned long)z->ty0, (unsigned long)(z->ty0 + z->th - 1));
        }
        mapfile_close(&mf);
        return 0;
    }
    double lat = atof(argv[2]), lon = atof(argv[3]);
    uint8_t zoom = atoi(argv[4]);
    ctx_t c = { .mf = &mf, .zoom = zoom };
    uint32_t tx = (uint32_t)(mapfile_lon_to_px(lon, zoom) / 256), ty = (uint32_t)(mapfile_lat_to_py(lat, zoom) / 256);
    if (argc < 6) {
        bool water;
        esp_err_t err = mapfile_read_tile(&mf, zoom, tx, ty, &water, dump_cb, &c);
        fprintf(stderr, "tile %lu/%lu z%u: err %d water %d ways %lu skipped %lu\n", (unsigned long)tx,
                (unsigned long)ty, zoom, err, water, (unsigned long)mf.ways, (unsigned long)mf.skipped);
        mapfile_close(&mf);
        return err != ESP_OK;
    }
    c.w = argc > 6 ? atoi(argv[6]) : 240;
    c.h = argc > 7 ? atoi(argv[7]) : 320;
    c.img = malloc(3 * c.w * c.h);
    memset(c.img, 0xf2, 3 * c.w * c.h);
    c.cx = mapfile_lon_to_px(lon, zoom);
    c.cy = mapfile_lat_to_py(lat, zoom);
    /* every tile touching the image; data comes per base tile, so the same
     * base tile may be visited more than once (harmless for a dump) */
    uint32_t tx0 = (uint32_t)((c.cx - c.w / 2) / 256), tx1 = (uint32_t)((c.cx + c.w / 2) / 256);
    uint32_t ty0 = (uint32_t)((c.cy - c.h / 2) / 256), ty1 = (uint32_t)((c.cy + c.h / 2) / 256);
    for (uint32_t y = ty0; y <= ty1; y++) {
        for (uint32_t x = tx0; x <= tx1; x++) {
            esp_err_t err = mapfile_read_tile(&mf, zoom, x, y, NULL, render_cb, &c);
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) fprintf(stderr, "tile %lu/%lu: err %d\n", (unsigned long)x, (unsigned long)y, err);
            c.tiles++;
        }
    }
    const uint8_t red[3] = { 255, 0, 0 };
    line(&c, c.w / 2 - 4, c.h / 2, c.w / 2 + 4, c.h / 2, red, 1);
    line(&c, c.w / 2, c.h / 2 - 4, c.w / 2, c.h / 2 + 4, red, 1);
    FILE *o = fopen(argv[5], "wb");
    fprintf(o, "P6\n%d %d\n255\n", c.w, c.h);
    fwrite(c.img, 3, c.w * c.h, o);
    fclose(o);
    fprintf(stderr, "%d tiles, %d way blocks, %d points -> %s\n", c.tiles, c.ways, c.points, argv[5]);
    mapfile_close(&mf);
    return 0;
}
