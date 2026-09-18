#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Reader for the vendor's vector maps (MAP/ directory on the eMMC).
 *
 * They are plain Mapsforge binary map files, version 3, produced with the
 * osmosis mapfile-writer (`--mw ... zoom-interval-conf=5,0,7,10,8,11,14,12,15
 * type=hd` with an iGPSPORT tag configuration): three zoom intervals with
 * base zooms 5 / 10 / 14, only ways (no POIs), a couple of dozen way tags
 * (highway=*, natural=water/coastline), street names but no debug info.
 * See https://github.com/mapsforge/mapsforge/blob/master/docs/Specification-Binary-Map-File.md
 *
 * Coordinates are microdegrees (1e-6 deg), WGS-84 in the files checked so
 * far (the "mars_" file name prefix notwithstanding); China maps may be
 * GCJ-02 shifted.
 *
 * The reader streams a tile way by way through a small buffer, so a tile of
 * any size costs a fixed amount of RAM (MAPFILE_WAY_BUF + the point arrays).
 * Ways larger than the buffer are skipped and counted in `skipped`. */

#define MAPFILE_MAX_ZOOM_INTERVALS 4
#define MAPFILE_MAX_TAGS           64
#define MAPFILE_WAY_BUF            8192   /* largest way record accepted, bytes */
#define MAPFILE_MAX_POINTS         2048   /* per coordinate block */
#define MAPFILE_STR_BUF            512    /* name + ref + house number of one way */

typedef struct {
    uint8_t base_zoom, min_zoom, max_zoom;
    uint64_t start;         /* sub-file start (file offset) */
    uint64_t size;
    uint32_t tx0, ty0;      /* tile range covered at base zoom */
    uint32_t tw, th;
} mapfile_zoom_interval_t;

typedef struct {
    FILE *f;
    int32_t min_lat, min_lon, max_lat, max_lon;   /* microdegrees */
    uint16_t tile_size;
    bool debug;                                    /* debug signatures present */
    uint8_t n_zoom;
    mapfile_zoom_interval_t zoom[MAPFILE_MAX_ZOOM_INTERVALS];
    uint8_t n_way_tags;
    char *way_tags[MAPFILE_MAX_TAGS];              /* "highway=primary" ... */
    uint8_t n_poi_tags;
    char *poi_tags[MAPFILE_MAX_TAGS];
    /* scratch (allocated in open, PSRAM when available) */
    uint8_t *way_buf;
    char *str_buf;
    int32_t *lat, *lon;
    /* statistics for the last mapfile_read_tile() */
    uint32_t ways, skipped;
} mapfile_t;

typedef struct {
    uint8_t tag_count;
    uint8_t tags[15];       /* indices into mapfile_t::way_tags */
    int8_t layer;           /* OSM layer, -5..10 */
    const char *name;       /* NULL if absent; valid during the callback only */
    const char *ref;
    const char *house_number;
    bool has_label_pos;
    int32_t label_lat, label_lon;
    uint16_t part;          /* way data block (independent polyline / polygon) */
    uint16_t block;         /* coordinate block index within the part */
    uint16_t n_blocks;      /* first block of a polygon = outer ring, rest = holes */
    uint16_t n_points;
    const int32_t *lat;     /* microdegrees */
    const int32_t *lon;
    uint16_t subtile_bitmap;/* which of the 4x4 sub-tiles the way touches (zoom base+2) */
} mapfile_way_t;

/* Called once per coordinate block of every way on the tile. */
typedef void (*mapfile_way_cb_t)(const mapfile_way_t *way, void *ctx);

esp_err_t mapfile_open(mapfile_t *mf, const char *path);
void mapfile_close(mapfile_t *mf);

/* Does the map cover this position? */
bool mapfile_contains(const mapfile_t *mf, int32_t lat_ud, int32_t lon_ud);

/* Zoom interval serving `zoom`, or NULL when out of range. */
const mapfile_zoom_interval_t *mapfile_interval(const mapfile_t *mf, uint8_t zoom);

/* Delivers every way stored for tile (tx, ty) at `zoom` (Web Mercator tile
 * numbers at that zoom). Only the ways whose minimum zoom is <= `zoom` are
 * reported. Data is stored per base-zoom tile: above the base zoom the ways
 * of the enclosing base tile are reported and extend beyond the requested
 * tile (the caller clips); below it every base tile inside the requested
 * tile is read. `*water` is set when the tile is entirely water.
 * ESP_ERR_NOT_FOUND when the tile lies outside the map, ESP_ERR_INVALID_SIZE
 * on a corrupt record. */
esp_err_t mapfile_read_tile(mapfile_t *mf, uint8_t zoom, uint32_t tx, uint32_t ty,
                            bool *water, mapfile_way_cb_t cb, void *ctx);

/* Lower level: one base tile (btx, bty at z->base_zoom) of a zoom interval,
 * ways with minimum zoom <= `zoom`. Renderers that know which base tiles
 * cover the screen call this directly. Statistics accumulate in mf->ways /
 * mf->skipped (reset them yourself). */
esp_err_t mapfile_read_base_tile(mapfile_t *mf, const mapfile_zoom_interval_t *z, uint32_t btx, uint32_t bty,
                                 uint8_t zoom, bool *water, mapfile_way_cb_t cb, void *ctx);

/* Web Mercator helpers (tile size 256 assumed by the projection formulas,
 * pixel coordinates are doubles so any tile size / scale can be derived). */
double mapfile_lon_to_px(double lon_deg, uint8_t zoom);   /* global pixel x at zoom */
double mapfile_lat_to_py(double lat_deg, uint8_t zoom);
double mapfile_px_to_lon(double px, uint8_t zoom);
double mapfile_py_to_lat(double py, uint8_t zoom);
