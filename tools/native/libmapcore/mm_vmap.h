/* MonoMesh vector map container (.mmap v2) - portable reader.
 *
 * Field by field format documentation lives in tools/native/vmap_write.c (the
 * writer) and is mirrored here.  Summary of the v2 additions:
 *
 *   header 128 B: same layout as v1 (tools/mono_map.py), version = 2,
 *                 flags bit 5 = FLAG_VECTOR, bits 16..31 = style_version
 *                 (the reader refuses a container whose style version differs
 *                 from the compiled libmapcore style)
 *   SEC_VBAND   (5): one per detail band (L/M/H), presence bitmap + row prefix
 *                    + 12 B chunk entries + chunk payloads
 *   SEC_POIGRID (6): label candidates bucketed on a slippy grid
 *   SEC_ZOOM/POI/STRINGS/PRECISION: unchanged from v1, so a v2 reader serves
 *                 raster v1 containers (basemap.mmap) through the same open()
 *
 * Everything this reader touches is untrusted input: counts, offsets and
 * sizes are validated with 64-bit arithmetic before any allocation or copy,
 * and the chunk decoder never reads past the buffer it was given.
 */
#ifndef MM_VMAP_H
#define MM_VMAP_H

#include "mm_core.h"

#define MM_VMAP_MAX_BANDS 3
#define MM_VMAP_MAX_ZOOMS 32
#define MM_VMAP_NAME_LEN 32
#define MM_VMAP_ATTRIB_LEN 48

/* section types */
#define MM_SEC_ZOOM 1
#define MM_SEC_POI 2
#define MM_SEC_STRINGS 3
#define MM_SEC_PREVIEW 4
#define MM_SEC_VBAND 5
#define MM_SEC_POIGRID 6

/* header flags (v1 bits plus v2 additions) */
#define MM_FLAG_DEFLATE 1
#define MM_FLAG_2BIT 2
#define MM_FLAG_PREVIEW 4
#define MM_FLAG_POI 8
#define MM_FLAG_DARK 16
#define MM_FLAG_VECTOR 32

#define MM_CHUNK_FLAG_RAW 1     /* payload is stored uncompressed */
#define MM_CHUNK_FLAG_SPLIT 2   /* reserved: adaptive 2x2 sub-cell */

/* Chunk-local coordinates are 1/16 px at grid_z: a cell is 4096 units wide,
 * plus margin.  The reader rejects records outside this range (a corrupt
 * container must not feed absurd coordinates to the rasteriser). */
#define MM_VMAP_COORD_MIN (-1024)
#define MM_VMAP_COORD_MAX 8192

#define MM_VBAND_SIZE 64        /* SEC_VBAND header */
#define MM_CHUNK_ENTRY_SIZE 12
#define MM_POIGRID_SIZE 64
#define MM_POI_CELL_ENTRY_SIZE 12
#define MM_POI_REF_SIZE 8

/* Absolute offset reader (SD file on the device, FILE* on the host).  Must
 * return the number of bytes read (short reads allowed at EOF) or < 0. */
typedef struct {
    int64_t (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    void *ctx;
    uint64_t size;
} mm_vmap_io;

typedef struct {
    uint8_t grid_z;          /* chunk grid zoom of the band */
    uint8_t zmin, zmax;      /* zoom range this band serves */
    uint8_t raw_default;     /* chunks stored raw when set */
    uint32_t x0, nx, y0, ny; /* chunk grid */
    uint32_t present_count;
    uint32_t payload_bytes;
    /* absolute file offsets of the index blocks */
    uint64_t off_presence, off_prefix, off_index, off_payload;
} mm_vmap_band;

typedef struct {
    uint8_t z, bits;
    uint32_t x0, nx, y0, ny;
    uint32_t present_count;
    uint32_t payload_bytes;
    uint64_t off_presence, off_prefix, off_index, off_payload;
} mm_vmap_zoom;

typedef struct {
    uint64_t offset;
    uint32_t length;
    uint16_t flags;
} mm_vmap_cell;

typedef struct {
    mm_vmap_io io;
    uint16_t version;
    uint16_t style_version;
    uint32_t flags;
    int32_t bbox_e7[4];
    char name[MM_VMAP_NAME_LEN + 1];
    char attribution[MM_VMAP_ATTRIB_LEN + 1];

    int n_bands;
    mm_vmap_band bands[MM_VMAP_MAX_BANDS];
    int n_zooms;
    mm_vmap_zoom zooms[MM_VMAP_MAX_ZOOMS];

    /* POI + strings */
    uint64_t off_poi, off_strings;
    uint32_t poi_count, strings_bytes;

    /* label candidate grid */
    int has_poigrid;
    uint8_t poigrid_z;
    uint32_t pg_x0, pg_nx, pg_y0, pg_ny, pg_present;
    uint64_t off_pg_presence, off_pg_prefix, off_pg_index, off_pg_refs;
    uint32_t pg_refs;
} mm_vmap;

/* Opens and fully validates the structure. Returns 0 ok, -1 refused/IO error. */
int mm_vmap_open(mm_vmap *m, const mm_vmap_io *io);

/* Chunk lookup (band index as returned in m->bands). Returns 0 when the cell
 * has data, 1 when the cell is empty, -1 on error. */
int mm_vmap_cell_info(const mm_vmap *m, int band, uint32_t cx, uint32_t cy,
                 mm_vmap_cell *out);

/* Reads + inflates one chunk payload into dst. Returns 0 ok, 1 empty, -1
 * error (dst is never written past dst_cap). */
int mm_vmap_read_chunk(mm_vmap *m, int band, uint32_t cx, uint32_t cy,
                       uint8_t *dst, size_t dst_cap, size_t *out_len,
                       uint16_t *chunk_flags);

/* POI access: one SEC_POI entry (v1 layout, 20 bytes). */
typedef struct {
    int32_t lat_e7, lon_e7;
    uint8_t kind;
    int32_t population;
    uint32_t name_off;
    uint16_t name_len;
} mm_vmap_poi;

int mm_vmap_poi_get(const mm_vmap *m, uint32_t index, mm_vmap_poi *out);
/* Copies the UTF-8 name into buf (NUL terminated). Returns its length. */
int mm_vmap_poi_name(const mm_vmap *m, const mm_vmap_poi *p, char *buf, size_t cap);

/* Label candidates: iterate the POI grid cells overlapping a slippy tile bbox
 * at poigrid_z.  cb receives (poi_index, score). */
typedef void (*mm_vmap_poi_cb)(void *user, uint32_t poi_index, int32_t score);
int mm_vmap_poi_foreach(const mm_vmap *m, uint32_t tx0, uint32_t ty0,
                        uint32_t tx1, uint32_t ty1, mm_vmap_poi_cb cb, void *user);

/* ---- chunk record decoder ---------------------------------------------- */

/* Per-record directory of one chunk payload (plan: decode the chunk once,
 * then cull whole records against the clip before touching their points).
 * A strip repaint pays the varint decode and the per-point transform only for
 * the records that really intersect it.
 *
 * Layout: 10 bytes per record in two arrays (no padding).  The bounds are
 * chunk-local in 2-grid-px units, floored/ceiled outward and clamped to the
 * cell, so they always CONTAIN the record's points (over-inclusion only costs
 * a few extra transforms; the exact cull stays in the stroke code).  Real
 * containers hold cells with ~90k tiny records: at 20 B/record a single
 * directory was megabytes, this layout keeps the hot cell under 900 KB. */
typedef struct {
    uint32_t *pos;    /* payload byte offset of each record header */
    uint8_t *meta;    /* 6 bytes/record: cls, flags, minx, miny, maxx, maxy */
    uint32_t n, cap;
} mm_chunk_dir;

#define MM_DIR_META_STRIDE 6
#define MM_DIR_BBOX_UNIT 32  /* chunk-local 1/16 px per encoded bound step */
#define MM_DIR_BYTES_PER_REC (sizeof(uint32_t) + MM_DIR_META_STRIDE)

/* Allocator injection keeps libmapcore dependency free (PSRAM on device,
 * malloc in the host tools). */
typedef void *(*mm_dir_alloc_fn)(void *ctx, size_t n);
typedef void (*mm_dir_free_fn)(void *ctx, void *p);

/* Builds the directory of one payload.  A corrupt record stops the walk with
 * the records decoded so far.  Returns:
 *   0  ok (dir may hold 0 records)
 *   1  over max_records: the directory is freed and must not be used (the
 *      caller falls back to the legacy walk).  Real containers have cells
 *      with ~90k tiny records: an unbounded directory is megabytes.
 *  -1  allocation failure (directory freed, caller falls back). */
int mm_chunk_dir_build(mm_chunk_dir *dir, const uint8_t *data, size_t len,
                       uint32_t max_records, mm_dir_alloc_fn alloc,
                       mm_dir_free_fn dealloc, void *ctx);
void mm_chunk_dir_free(mm_chunk_dir *dir, mm_dir_free_fn dealloc, void *ctx);

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    uint32_t remaining;  /* points left in the current record */
    uint8_t cls, flags;
    int32_t x, y;        /* current point, chunk-local 1/16 px at grid_z */
    int error;
} mm_chunk_reader;

void mm_chunk_reader_init(mm_chunk_reader *r, const uint8_t *data, size_t len);
/* Next record: 0 ok, 1 end of chunk, -1 corrupt. */
int mm_chunk_next_record(mm_chunk_reader *r);
/* Next point of the current record: 1 ok, 0 record exhausted, -1 corrupt. */
int mm_chunk_next_point(mm_chunk_reader *r);

#endif /* MM_VMAP_H */
