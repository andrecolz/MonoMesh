/* MonoMesh native tile generator - shared declarations.
 *
 * The data model and the on-disk cache format are shared with the Python
 * implementation (tools/mmap_converter.py), so caches can be produced by one
 * tool and consumed by the other.
 */
#ifndef MONOMESH_H
#define MONOMESH_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MM_CACHE_MAGIC "MOSMTILE1\0"
#define MM_CACHE_MAGIC_LEN 10
/* 3: water rings carry a meaningful "closed" flag (fillable vs outline only),
 *    so caches written before this change are regenerated. */
#define MM_CACHE_VERSION 3
#define MM_E5 100000.0

/* way type codes (must match the Python implementation) */
enum { MM_HIGHWAY = 0, MM_WATER = 1, MM_RAILWAY = 2 };

/* water sub-kinds, stored in `prio` for water ways (must match Python) */
enum {
    MM_WATER_STREAM = 0,      /* stream / ditch / drain */
    MM_WATER_RIVER = 1,       /* river / canal */
    MM_WATER_AREA = 2,        /* natural=water / riverbank */
    MM_WATER_RING_OUTER = 3,  /* outer ring of a water multipolygon */
    MM_WATER_RING_INNER = 4,  /* inner ring (island) */
};

#define MM_PRIO_NONE 255

/* place kind indexes and font variants live in libmapcore/mm_style.h (the
 * canonical style header shared with the firmware) */
#include "libmapcore/mm_style.h"

typedef struct {
    uint8_t type;
    uint8_t prio;
    uint8_t closed;
    int32_t npts;
    int32_t off; /* offset into lat_e5/lon_e5 */
} mm_way;

typedef struct {
    double bbox[4]; /* min_lat, min_lon, max_lat, max_lon */
    int32_t n_ways;
    int32_t n_places;
    int32_t n_points;
    mm_way *ways;
    int32_t *lat_e5;
    int32_t *lon_e5;
    int32_t *place_lat_e5;
    int32_t *place_lon_e5;
    uint8_t *place_kind;
    int32_t *place_population;   /* cache v2: OSM population tag (0 = unknown) */
    int32_t *name_off;
    uint16_t *name_len;
    char *names;
    uint32_t names_len;
} mm_data;

/* ---------------------------------------------------------------- style -- */
int mm_road_prio(const char *cls);                /* -1 when not drawn */
const char *mm_road_name(int prio);
/* width of a road class at a zoom; 0 when the class is not shown at all */
int mm_road_width(int prio, int z, int *has_casing);
int mm_water_line_width(int z);
/* minimum projected size (px) for a water area to be drawn at this zoom */
double mm_water_min_size(int z);
/* water kind visible at this zoom? */
int mm_water_kind_drawn(int kind, int z);
/* label policy */
int mm_label_kind_drawn(int kind, int z);
int mm_label_budget(int z);
int mm_label_score(int kind, int32_t population);

/* ------------------------------------------------------------------ pbf -- */
/* Extracts ways/places inside ``filter_bbox`` (the caller passes the already
 * expanded bounding box) and stores ``data_bbox`` as the area of the resulting
 * dataset (that is what the tile grid is derived from). Returns 0 on success. */
int mm_pbf_extract(const char *path, const double filter_bbox[4], const double data_bbox[4],
                   mm_data *out, int threads);

/* ---------------------------------------------------------------- cache -- */
int mm_cache_write(const char *path, const mm_data *d);
int mm_cache_read(const char *path, mm_data *d);
void mm_data_free(mm_data *d);

/* --------------------------------------------------------------- render -- */
typedef struct mm_index mm_index;

typedef struct {
    int32_t tx0, ty0; /* tile grid origin of the index */
    int32_t nx, ny;
} mm_grid;

mm_grid mm_tile_grid(const mm_data *d, int z);

/* Projected world coordinates (tile units) for every point at ``ref_z``,
 * interleaved as x,y,x,y,...  Computed once, then rescaled exactly (power of
 * two) for the other zoom levels. */
double *mm_project_points(const mm_data *d, int ref_z);

mm_index *mm_index_build(const mm_data *d, int z, int ref_z, const double *xy);
void mm_index_free(mm_index *ix);

int mm_render_tile_png(mm_index *ix, int z, int tx, int ty, int style_dark,
                       int png_bits, const char *out_path, size_t *out_size);

/* Draws one tile into a caller-provided 256x256 greyscale buffer
 * (used by the PNG path and by the benchmarks/tests). */
void mm_render_tile_img(mm_index *ix, int z, int tx, int ty, int style_dark, uint8_t *img);

/* --------------------------------------------------------------- common -- */
int mm_mkpath(const char *path);
double mm_now(void);

#endif /* MONOMESH_H */
