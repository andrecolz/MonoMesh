/* MonoMesh vector map container (.mmap v2) - writer, verifier, inspector.
 *
 * ==========================================================================
 * FORMAT v2 - field by field (the reader is libmapcore/mm_vmap.c)
 * ==========================================================================
 *
 * File layout (as v1, see tools/mono_map.py):
 *
 *     header           128 bytes
 *     section table    section_count * 16 bytes
 *     sections         in the same order as the table
 *     payload          chunk payloads, 4-byte aligned
 *
 * Header (128 bytes, same offsets as v1 so a v2 reader serves v1 files)::
 *
 *     0   8s  magic "MONOMMAP"
 *     8   H   version (= 2)
 *     10  H   header_size (128)
 *     12  I   flags   bit0 DEFLATE, bit1 2BIT, bit2 PREVIEW, bit3 POI,
 *                     bit4 DARK, bit5 VECTOR, bits 16..31 style_version
 *                     (MM_STYLE_VERSION of libmapcore/style.c: the reader
 *                     refuses a container built against another style)
 *     16  I   tile_px (256)
 *     20  B   payload_bits (1 or 2, raster sections only)
 *     21  B   zoom_count (number of SEC_ZOOM raster sections)
 *     22  H   section_count
 *     24  I   section_table_off (128)
 *     28  I   file_size
 *     32  4i  region bbox in degrees * 1e7 (min_lat, min_lon, max_lat, max_lon)
 *     48  32s region name (UTF-8, NUL padded)
 *     80  48s attribution (UTF-8, NUL padded)
 *
 * Section table entry (16 bytes, unchanged)::
 *
 *     0   I   type (SEC_ZOOM 1, SEC_POI 2, SEC_STRINGS 3, SEC_PREVIEW 4,
 *                   SEC_VBAND 5, SEC_POIGRID 6)
 *     4   I   offset (from file start)
 *     8   I   size
 *     12  I   aux (SEC_ZOOM: zoom; SEC_POI: entry count; SEC_VBAND: band
 *                  index; SEC_POIGRID: reserved)
 *
 * SEC_VBAND (64 byte header + arrays, one section per detail band)::
 *
 *     0   B   band            (0 = L z7-11, 1 = M z12-14, 2 = H z15+)
 *     1   B   grid_z          (chunk grid zoom: slippy tiles of 256 px)
 *     2   H   flags           (CHUNK_FLAG_RAW: default chunk storage)
 *     4   I   x0 / 8 nx / 12 y0 / 16 ny   (chunk grid)
 *     20  I   presence_off    nx*ny bits, row major, MSB first (relative to
 *                             the section start, like v1)
 *     24  I   row_prefix_off  (ny+1) * u32 cumulative present cells per row
 *     28  I   index_off       present_count * CHUNK_ENTRY (12 bytes)
 *     32  I   payload_off     first chunk payload (absolute)
 *     36  I   payload_size    total bytes of the chunk payloads
 *     40  I   present_count
 *     44  B   zmin / 45 B zmax (zoom range served by the band)
 *     46  H   tol_px_16       (Douglas-Peucker tolerance at zmax, 1/16 px)
 *     48  16x reserved (zero)
 *
 * CHUNK_ENTRY (12 bytes)::
 *
 *     0   I   payload offset (absolute, 0 when the cell is blank)
 *     4   I   payload length (0 when blank)
 *     8   H   flags (CHUNK_FLAG_RAW: payload is not deflated;
 *                    CHUNK_FLAG_SPLIT: reserved for adaptive 2x2 sub-cells)
 *     10  H   reserved
 *
 * Chunk payload (one zlib stream, or raw when CHUNK_FLAG_RAW; the generator
 * stores the payload uncompressed when zlib does not save at least 5% -
 * measured on band H where varint data expands by 4%)::
 *
 *     record*            (in draw order, see below)
 *   record::
 *     u8   class_flags     (low 5 bits: MM_CLS_*; high 3 bits: MM_REC_FILL,
 *                           MM_REC_BUTT_START, MM_REC_BUTT_END)
 *     u8   npts_hint       (npts - 2 for records up to 254 points;
 *                           255 = escape, an unsigned LEB128 count follows.
 *                           Long ways are split at generation time, 1024
 *                           points max per record)
 *     i16   x0, i16 y0     (chunk local coordinates, 1/16 px at grid_z, little
 *                           endian, relative to the chunk's top left corner;
 *                           bounded by the cell + margin, see MM_VMAP_COORD_MAX)
 *     (varint dx, varint dy) * (npts - 1)   zigzag signed deltas
 *
 *   Draw order inside a chunk: water fills, island holes, water lines, roads
 *   minor -> major, railways last (plan D2; stable order, deterministic).
 *
 *   Boundary sharing: a way crossing a chunk border is split exactly on the
 *   border line and the shared point is written with the same coordinate in
 *   both chunks (one clip in one coordinate space), so the two butt caps meet
 *   without a seam.  MM_REC_BUTT_* marks the artificial ends (mid-segment
 *   splits): the renderer draws no round join there.  An end that falls on a
 *   real vertex keeps its join.
 *
 * SEC_POI (20 byte entries, unchanged from v1) and SEC_STRINGS (concatenated
 * UTF-8 names) are reused as-is.
 *
 * SEC_POIGRID (64 byte header + arrays; label candidates bucketed on a slippy
 * grid, refs sorted by score inside every cell so the device walks them in
 * importance order)::
 *
 *     0   B   grid_z / 1 B reserved / 2 H flags
 *     4   I   x0 / 8 nx / 12 y0 / 16 ny
 *     20  I   presence_off    (relative)
 *     24  I   row_prefix_off  (relative)
 *     28  I   index_off       present_count * POI_CELL_ENTRY (12 bytes)
 *     32  I   refs_off        refs_count * POI_REF (8 bytes, relative)
 *     36  I   refs_size       bytes of the refs array
 *     40  I   present_count
 *     44  I   refs_count
 *     48  16x reserved
 *
 *   POI_CELL_ENTRY (12 bytes): 0 I first ref, 4 I ref count, 8 I reserved
 *   POI_REF (8 bytes): 0 I poi index (into SEC_POI), 4 i score (precomputed
 *   with mm_style_label_score)
 */
#include "monomesh.h"

#include "libmapcore/mm_core.h"
#include "libmapcore/mm_geom.h"
#include "libmapcore/mm_style.h"
#include "libmapcore/mm_varint.h"
#include "libmapcore/mm_vmap.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static int g_quiet;

static void logi(const char *fmt, ...)
{
    va_list ap;
    if (g_quiet)
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static double now_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ bands */

typedef struct {
    int index;
    const char *name;
    int zmin, zmax;
    int grid_z;
    double tol_px_zmax; /* Douglas-Peucker tolerance in px at zmax */
    int road_min_cls;   /* highways with class id >= this are kept */
    int skip_path;      /* path is z15+ in the style: bands below H drop it */
    int keep_rail, keep_stream, keep_area, keep_river;
} band_cfg;

/* Plan D-0.1/D-0.2/D-0.3 (scratch/spike_piano_b.md).  The class filter is the
 * union of what the shared style draws inside the band's zoom range (plan D3:
 * the style is the single source of truth), so no road class the style shows
 * can silently go missing: L keeps secondary because the style draws it from
 * z10, M keeps every class but path (style: z15+). */
static const band_cfg BANDS[3] = {
    {0, "L", 7, 11, 8, 2.0, MM_CLS_SECONDARY, 0, 1, 0, 1, 1},
    {1, "M", 12, 14, 13, 2.0, MM_CLS_PEDESTRIAN, 1, 1, 1, 1, 1},
    {2, "H", 15, 18, 15, 4.0, MM_CLS_PEDESTRIAN, 0, 1, 1, 1, 1},
};

#define POIGRID_Z 13
#define MAX_REC_PTS 1024

static int way_class(const mm_way *w)
{
    switch (w->type) {
    case MM_HIGHWAY:
        return w->prio < 17 ? w->prio : -1;
    case MM_WATER:
        switch (w->prio) {
        case MM_WATER_STREAM: return MM_CLS_WATER_STREAM;
        case MM_WATER_RIVER: return MM_CLS_WATER_RIVER;
        case MM_WATER_AREA: return MM_CLS_WATER_AREA;
        case MM_WATER_RING_OUTER: return MM_CLS_WATER_RING_OUTER;
        case MM_WATER_RING_INNER: return MM_CLS_WATER_RING_INNER;
        }
        return -1;
    case MM_RAILWAY:
        return MM_CLS_RAILWAY;
    }
    return -1;
}

static int way_in_band(const band_cfg *b, int cls)
{
    if (cls < 0)
        return 0;
    if (cls <= MM_CLS_MOTORWAY_LINK) {
        if (b->skip_path && cls == MM_CLS_PATH)
            return 0;
        return cls >= b->road_min_cls;
    }
    switch (cls) {
    case MM_CLS_WATER_STREAM: return b->keep_stream;
    case MM_CLS_WATER_RIVER: return b->keep_river;
    case MM_CLS_WATER_AREA: return b->keep_area;
    case MM_CLS_WATER_RING_OUTER:
    case MM_CLS_WATER_RING_INNER: return 1;
    case MM_CLS_RAILWAY: return b->keep_rail;
    }
    return 0;
}

/* ------------------------------------------------------------- cell build */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} bytebuf;

static void bb_put(bytebuf *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n)
            cap *= 2;
        b->buf = (uint8_t *)realloc(b->buf, cap);
        if (!b->buf) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        b->cap = cap;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
}

typedef struct {
    uint32_t off;  /* record offset inside the cell buffer */
    uint32_t len;
    int order;     /* draw order key */
    uint32_t seq;  /* insertion order, stable tie break */
} rec_desc;

typedef struct {
    bytebuf data;
    rec_desc *recs;
    size_t n_recs, cap_recs;
    uint16_t chunk_flags; /* CHUNK_FLAG_RAW when stored uncompressed */
} cell_build;

static void cell_add_record(cell_build *c, int cls, uint8_t flags,
                            const mm_pt *pts, int n)
{
    size_t start = c->data.len;
    unsigned char hdr[16];
    int hl = 0;
    hdr[hl++] = (unsigned char)((cls & 0x1f) | ((flags & 0x07) << 5));
    if (n - 2 < MM_REC_NPTS_ESCAPE) {
        hdr[hl++] = (unsigned char)(n - 2);
    } else {
        hdr[hl++] = MM_REC_NPTS_ESCAPE;
        hl += mm_varint_put(hdr + hl, (uint32_t)n);
    }
    bb_put(&c->data, hdr, (size_t)hl);
    /* first point absolute in chunk-local 1/16 px: int16 is plenty (the cell
     * is 4096 units wide plus a 512 unit margin) and saves 4 bytes on every
     * record - measured 27% of the H payload with int32 */
    unsigned char pnt[16];
    int16_t x0 = (int16_t)pts[0].x, y0 = (int16_t)pts[0].y;
    memcpy(pnt, &x0, 2);
    memcpy(pnt + 2, &y0, 2);
    bb_put(&c->data, pnt, 4);
    for (int i = 1; i < n; i++) {
        int off = 0;
        off += mm_varint_put(pnt + off, mm_zigzag_enc(pts[i].x - pts[i - 1].x));
        off += mm_varint_put(pnt + off, mm_zigzag_enc(pts[i].y - pts[i - 1].y));
        bb_put(&c->data, pnt, (size_t)off);
    }
    if (c->n_recs == c->cap_recs) {
        size_t cap = c->cap_recs ? c->cap_recs * 2 : 64;
        c->recs = (rec_desc *)realloc(c->recs, sizeof(rec_desc) * cap);
        if (!c->recs) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        c->cap_recs = cap;
    }
    rec_desc *r = &c->recs[c->n_recs++];
    r->off = (uint32_t)start;
    r->len = (uint32_t)(c->data.len - start);
    r->order = mm_style_draw_order(cls);
    r->seq = (uint32_t)c->n_recs;
}

static int cmp_rec_desc(const void *a, const void *b)
{
    const rec_desc *x = a, *y = b;
    if (x->order != y->order)
        return x->order < y->order ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

/* Reorders the cell buffer by draw order (stable). */
static void cell_sort(cell_build *c)
{
    if (c->n_recs < 2)
        return;
    qsort(c->recs, c->n_recs, sizeof(rec_desc), cmp_rec_desc);
    uint8_t *out = (uint8_t *)malloc(c->data.len ? c->data.len : 1);
    if (!out) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    size_t pos = 0;
    for (size_t i = 0; i < c->n_recs; i++) {
        memcpy(out + pos, c->data.buf + c->recs[i].off, c->recs[i].len);
        pos += c->recs[i].len;
    }
    free(c->data.buf);
    c->data.buf = out;
    c->data.len = pos;
    c->data.cap = pos;
}

/* Compresses when zlib saves at least 5% (decision D-0.5). */
static void cell_compress(cell_build *c, z_stream *zs)
{
    if (c->data.len == 0)
        return;
    uLongf bound = compressBound((uLong)c->data.len);
    uint8_t *zbuf = (uint8_t *)malloc(bound ? bound : 1);
    if (!zbuf) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    zs->next_in = c->data.buf;
    zs->avail_in = (uInt)c->data.len;
    zs->next_out = zbuf;
    zs->avail_out = (uInt)bound;
    if (deflate(zs, Z_FINISH) != Z_STREAM_END) {
        fprintf(stderr, "deflate failed\n");
        exit(1);
    }
    size_t zlen = zs->total_out;
    deflateReset(zs);
    if (zlen * 20 < c->data.len * 19) { /* >= 5% saving, integer exact */
        free(c->data.buf);
        c->data.buf = zbuf;
        c->data.len = zlen;
        c->data.cap = bound;
        c->chunk_flags = 0;
    } else {
        free(zbuf);
        c->chunk_flags = MM_CHUNK_FLAG_RAW;
    }
}

/* ------------------------------------------------------- band generation -- */

typedef struct {
    double x, y;
} dpt;

typedef struct {
    int64_t ways_in, pts_in, pts_out, recs;
} band_stats;

/* Liang-Barsky in double: returns the inside portion of a segment.  The clip
 * is done in one shared coordinate space per way, so a border point gets the
 * same 1/16 px value in both adjacent chunks (no seam). */
static int dlb_clip(dpt *a, dpt *b, double x0, double y0, double x1, double y1)
{
    double t0 = 0.0, t1 = 1.0;
    double dx = b->x - a->x, dy = b->y - a->y;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a->x - x0, x1 - a->x, a->y - y0, y1 - a->y};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0)
                return 0;
            continue;
        }
        double r = q[i] / p[i];
        if (p[i] < 0.0) {
            if (r > t1)
                return 0;
            if (r > t0)
                t0 = r;
        } else {
            if (r < t0)
                return 0;
            if (r < t1)
                t1 = r;
        }
    }
    dpt na = {a->x + t0 * dx, a->y + t0 * dy};
    dpt nb = {a->x + t1 * dx, a->y + t1 * dy};
    *a = na;
    *b = nb;
    return 1;
}

/* Sutherland-Hodgman polygon clip against a convex rect: the output is closed
 * along the border (plan D2: "anelli acqua clippati devono restare chiusi, o
 * il fill esplode") so a lake split across cells fills correctly in every
 * cell.  Returns the vertex count written to out (0 when fully outside).  The
 * buffers are heap sized: real coastlines carry 30k+ vertices. */
static int sh_clip(const dpt *pts, int n, double rx0, double ry0, double rx1,
                   double ry1, dpt *out, int max_out)
{
    if (n < 3)
        return 0;
    /* one clip pass can add up to n crossings, so the ping-pong buffers must
     * hold 2n+16 vertices (an n+8 buffer overflows on comb shaped rings) */
    int cap = 2 * n + 16;
    dpt *a = (dpt *)malloc(sizeof(dpt) * (size_t)cap);
    dpt *b = (dpt *)malloc(sizeof(dpt) * (size_t)cap);
    if (!a || !b) {
        free(a);
        free(b);
        return 0;
    }
    memcpy(a, pts, sizeof(dpt) * (size_t)n);
    int na = n;

    for (int edge = 0; edge < 4 && na > 0; edge++) {
        int nb = 0;
        for (int i = 0; i < na; i++) {
            dpt cur = a[i], nxt = a[(i + 1) % na];
            double vc = 0, vn = 0;
            switch (edge) {
            case 0: vc = cur.x - rx0; vn = nxt.x - rx0; break;
            case 1: vc = rx1 - cur.x; vn = rx1 - nxt.x; break;
            case 2: vc = cur.y - ry0; vn = nxt.y - ry0; break;
            default: vc = ry1 - cur.y; vn = ry1 - nxt.y; break;
            }
            int cin = vc >= 0, nin = vn >= 0;
            if (cin) {
                if (nb >= cap)
                    goto fail;
                b[nb++] = cur;
            }
            if (cin != nin) {
                /* intersection with the edge line */
                if (nb >= cap)
                    goto fail;
                double t = vc / (vc - vn);
                dpt p;
                p.x = cur.x + t * (nxt.x - cur.x);
                p.y = cur.y + t * (nxt.y - cur.y);
                b[nb++] = p;
            }
        }
        memcpy(a, b, sizeof(dpt) * (size_t)nb);
        na = nb;
    }
    int out_n = 0;
    if (na >= 3 && na <= max_out) {
        memcpy(out, a, sizeof(dpt) * (size_t)na);
        out_n = na;
    } else if (na > max_out) {
        fprintf(stderr, "sh_clip: ring piece %d exceeds %d vertices\n", na, max_out);
    }
    free(a);
    free(b);
    return out_n;

fail:
    /* adversarial geometry must never corrupt the heap: drop the piece */
    fprintf(stderr, "sh_clip: vertex overflow on %d vertex ring, dropped\n", n);
    free(a);
    free(b);
    return 0;
}

static double mercator_x(double lon, double n)
{
    return (lon + 180.0) / 360.0 * n * 256.0;
}

static double mercator_y(double lat, double n)
{
    return (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * n * 256.0;
}

/* Clips one simplified polyline to one cell and emits its runs.  A run end
 * that coincides with an original vertex keeps its round join (the way was
 * split there), a mid-segment end is artificial: butt cap, no join. */
static void clip_to_cell(band_stats *st, cell_build *cell, int cls,
                         uint8_t base_flags, const dpt *pts, int n,
                         double rx0, double ry0, double rx1, double ry1)
{
    mm_pt run[MAX_REC_PTS + 2];
    int run_n = 0, run_start_vertex = 0, run_end_vertex = 0;
    dpt prev_out;
    int have_prev = 0;

#define FLUSH_RUN()                                                          \
    do {                                                                     \
        if (run_n >= 2) {                                                    \
            uint8_t fl = base_flags;                                         \
            if (!run_start_vertex)                                           \
                fl |= MM_REC_BUTT_START;                                     \
            if (!run_end_vertex)                                             \
                fl |= MM_REC_BUTT_END;                                       \
            cell_add_record(cell, cls, fl, run, run_n);                       \
            st->recs++;                                                      \
        }                                                                    \
        run_n = 0;                                                           \
        have_prev = 0;                                                       \
    } while (0)

    for (int i = 0; i + 1 < n; i++) {
        dpt a = pts[i], b = pts[i + 1];
        if (!dlb_clip(&a, &b, rx0, ry0, rx1, ry1)) {
            FLUSH_RUN();
            continue;
        }
        /* the clipped point still equals the original vertex when the clip
         * parameter was 0 or 1: that is what separates a real vertex (keep
         * the round join) from an artificial end (butt cap) */
        int a_vertex = (a.x == pts[i].x && a.y == pts[i].y);
        int b_vertex = (b.x == pts[i + 1].x && b.y == pts[i + 1].y);
        int continues = have_prev && a.x == prev_out.x && a.y == prev_out.y;
        if (!continues) {
            FLUSH_RUN();
            run[0].x = (int32_t)lround((a.x - rx0) * 16.0); /* 1/16 px units */
            run[0].y = (int32_t)lround((a.y - ry0) * 16.0);
            run_n = 1;
            run_start_vertex = a_vertex;
        }
        if (run_n >= MAX_REC_PTS) {
            /* record limit (1024 points, spec): split at the last vertex,
             * which keeps its round join */
            uint8_t fl = base_flags;
            if (!run_start_vertex)
                fl |= MM_REC_BUTT_START;
            cell_add_record(cell, cls, fl, run, run_n);
            st->recs++;
            run[0] = run[run_n - 1];
            run_n = 1;
            run_start_vertex = 1;
        }
        run[run_n].x = (int32_t)lround((b.x - rx0) * 16.0);
        run[run_n].y = (int32_t)lround((b.y - ry0) * 16.0);
        run_n++;
        run_end_vertex = b_vertex;
        prev_out = b;
        have_prev = 1;
    }
    FLUSH_RUN();
#undef FLUSH_RUN
}

/* Emits one closed water ring piece for a cell.  A piece with more than
 * MM_FILL_MAX_PTS vertices (detailed coastlines) is split by bisecting the
 * clip rect and clipping the ring again: the pieces tile exactly the same
 * area (rect clipping is exact), stay closed along their borders, and each is
 * small enough for the renderer's fixed arena. */
#define MM_FILL_MAX_PTS 1200

static void emit_fill_ring(band_stats *st, cell_build *cell, int cls,
                           uint8_t base_flags, const dpt *pts, int n,
                           double rx0, double ry0, double rx1, double ry1,
                           int depth)
{
    int cap = MM_FILL_MAX_PTS + 8;
    dpt *poly = (dpt *)malloc(sizeof(dpt) * (size_t)cap);
    if (!poly)
        return;
    int pn = sh_clip(pts, n, rx0, ry0, rx1, ry1, poly, cap);
    if (pn >= 3 && pn <= MM_FILL_MAX_PTS) {
        mm_pt *ring = (mm_pt *)malloc(sizeof(mm_pt) * (size_t)pn);
        if (ring) {
            for (int i = 0; i < pn; i++) {
                ring[i].x = (int32_t)lround((poly[i].x - rx0) * 16.0);
                ring[i].y = (int32_t)lround((poly[i].y - ry0) * 16.0);
            }
            cell_add_record(cell, cls, base_flags, ring, pn);
            st->recs++;
            free(ring);
        }
    } else if (pn >= 3 && depth < 24) {
        /* too many vertices for one record: bisect the longer side */
        double mx = (rx0 + rx1) / 2.0, my = (ry0 + ry1) / 2.0;
        if (rx1 - rx0 >= ry1 - ry0) {
            emit_fill_ring(st, cell, cls, base_flags, pts, n, rx0, ry0, mx, ry1, depth + 1);
            emit_fill_ring(st, cell, cls, base_flags, pts, n, mx, ry0, rx1, ry1, depth + 1);
        } else {
            emit_fill_ring(st, cell, cls, base_flags, pts, n, rx0, ry0, rx1, my, depth + 1);
            emit_fill_ring(st, cell, cls, base_flags, pts, n, rx0, my, rx1, ry1, depth + 1);
        }
    } else if (pn >= 3) {
        fprintf(stderr, "emit_fill_ring: piece still %d vertices at depth %d, dropped\n",
                pn, depth);
    }
    free(poly);
}

static void build_band(const mm_data *d, const band_cfg *cfg, cell_build *cells,
                       int32_t nx, int32_t ny, int32_t tx0, int32_t ty0,
                       band_stats *st)
{
    memset(st, 0, sizeof(*st));
    const double n2 = pow(2.0, cfg->grid_z);
    double tol_px = cfg->tol_px_zmax * pow(2.0, cfg->grid_z - cfg->zmax);
    const double unit = 16.0; /* storage units per px at grid_z */

    dpt *pts = (dpt *)malloc(sizeof(dpt) * (size_t)(d->n_points + 2));
    dpt *simp = (dpt *)malloc(sizeof(dpt) * (size_t)(d->n_points + 2));
    mm_pt *local = (mm_pt *)malloc(sizeof(mm_pt) * (size_t)(d->n_points + 2));
    mm_pt *lsimp = (mm_pt *)malloc(sizeof(mm_pt) * (size_t)(d->n_points + 2));
    if (!pts || !simp || !local || !lsimp) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    int32_t tol16 = (int32_t)lround(tol_px * unit);
    if (tol16 < 1)
        tol16 = 1;

    for (int32_t wi = 0; wi < d->n_ways; wi++) {
        const mm_way *w = &d->ways[wi];
        int cls = way_class(w);
        if (!way_in_band(cfg, cls) || w->npts < 2)
            continue;
        st->ways_in++;
        st->pts_in += w->npts;

        double wx0 = mercator_x(d->lon_e5[w->off] / MM_E5, n2);
        double wy0 = mercator_y(d->lat_e5[w->off] / MM_E5, n2);
        int32_t n = w->npts;
        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for (int32_t j = 0; j < n; j++) {
            double x = mercator_x(d->lon_e5[w->off + j] / MM_E5, n2);
            double y = mercator_y(d->lat_e5[w->off + j] / MM_E5, n2);
            pts[j].x = x;
            pts[j].y = y;
            local[j].x = (int32_t)lround((x - wx0) * unit);
            local[j].y = (int32_t)lround((y - wy0) * unit);
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
        int sn = mm_dp_simplify(local, n, tol16 * tol16, lsimp, NULL, NULL);
        if (sn < 2)
            continue;
        for (int32_t j = 0; j < sn; j++) {
            simp[j].x = wx0 + lsimp[j].x / unit;
            simp[j].y = wy0 + lsimp[j].y / unit;
        }
        uint8_t base_flags = 0;
        if (w->closed && sn >= 3) {
            simp[sn] = simp[0];
            sn++;
            if (w->type == MM_WATER &&
                (w->prio == MM_WATER_AREA || w->prio == MM_WATER_RING_OUTER ||
                 w->prio == MM_WATER_RING_INNER))
                base_flags |= MM_REC_FILL;
        }
        st->pts_out += sn;

        int cx0 = (int)floor(minx / 256.0) - tx0;
        int cx1 = (int)floor(maxx / 256.0) - tx0;
        int cy0 = (int)floor(miny / 256.0) - ty0;
        int cy1 = (int)floor(maxy / 256.0) - ty0;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > nx - 1) cx1 = nx - 1;
        if (cy1 > ny - 1) cy1 = ny - 1;

        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                double rx0 = (double)(tx0 + cx) * 256.0;
                double ry0 = (double)(ty0 + cy) * 256.0;
                cell_build *cell = &cells[(size_t)cy * nx + cx];
                if (base_flags & MM_REC_FILL) {
                    /* closed water rings: Sutherland-Hodgman keeps the piece
                     * closed along the cell border (plan D2: clipped rings
                     * must stay closed or the fill explodes), so a lake split
                     * across cells fills correctly in every cell */
                    emit_fill_ring(st, cell, cls, base_flags, simp, sn, rx0, ry0,
                                   rx0 + 256.0, ry0 + 256.0, 0);
                } else {
                    clip_to_cell(st, cell, cls, base_flags, simp, sn, rx0, ry0,
                                 rx0 + 256.0, ry0 + 256.0);
                }
            }
        }
    }
    free(pts);
    free(simp);
    free(local);
    free(lsimp);
}

/* --------------------------------------------------------------- packing -- */

typedef struct {
    int32_t nx, ny, tx0, ty0;
    cell_build *cells;
    uint32_t present;
    uint64_t payload_bytes;
} band_out;

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }

typedef struct {
    uint32_t poi;
    int32_t score;
} poi_ref;

static int cmp_poi_ref(const void *a, const void *b)
{
    const poi_ref *x = a, *y = b;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    return x->poi < y->poi ? -1 : (x->poi > y->poi ? 1 : 0);
}

static int generate(const char *cache_path, const char *out_path, const char *name,
                    double l_tol, double m_tol, double h_tol)
{
    double t_all = now_secs();
    band_cfg bands[3];
    memcpy(bands, BANDS, sizeof(bands));
    bands[0].tol_px_zmax = l_tol;
    bands[1].tol_px_zmax = m_tol;
    bands[2].tol_px_zmax = h_tol;
    mm_data d;
    logi("[plan] source cache : %s\n", cache_path);
    logi("[plan] bands        : L z%d-%d grid z%d tol %.1fpx | M z%d-%d grid z%d "
         "tol %.1fpx | H z%d-%d grid z%d tol %.1fpx\n",
         bands[0].zmin, bands[0].zmax, bands[0].grid_z, bands[0].tol_px_zmax,
         bands[1].zmin, bands[1].zmax, bands[1].grid_z, bands[1].tol_px_zmax,
         bands[2].zmin, bands[2].zmax, bands[2].grid_z, bands[2].tol_px_zmax);
    logi("[plan] POI grid     : z%d  chunk records <= %d points\n", POIGRID_Z,
         MAX_REC_PTS);
    if (mm_cache_read(cache_path, &d) != 0) {
        fprintf(stderr, "cannot read cache %s\n", cache_path);
        return 1;
    }
    logi("[plan] ways %d  points %d  places %d\n", d.n_ways, d.n_points, d.n_places);
    double t_load = now_secs();

    band_out bo[3];
    memset(bo, 0, sizeof(bo));
    for (int b = 0; b < 3; b++) {
        const band_cfg *cfg = &bands[b];
        double n2 = pow(2.0, cfg->grid_z);
        int tx0 = (int)floor(mercator_x(d.bbox[1], n2) / 256.0);
        int tx1 = (int)floor(mercator_x(d.bbox[3], n2) / 256.0);
        int ty0 = (int)floor(mercator_y(d.bbox[2], n2) / 256.0);
        int ty1 = (int)floor(mercator_y(d.bbox[0], n2) / 256.0);
        bo[b].tx0 = tx0;
        bo[b].ty0 = ty0;
        bo[b].nx = tx1 - tx0 + 1;
        bo[b].ny = ty1 - ty0 + 1;
        if (bo[b].nx <= 0 || bo[b].ny <= 0 ||
            (int64_t)bo[b].nx * bo[b].ny > (1 << 22)) {
            fprintf(stderr, "band %s: bad grid\n", cfg->name);
            return 1;
        }
        bo[b].cells = (cell_build *)calloc((size_t)bo[b].nx * bo[b].ny,
                                           sizeof(cell_build));
        if (!bo[b].cells) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
    }

    /* ---- geometry ------------------------------------------------------ */
    for (int b = 0; b < 3; b++) {
        double tb = now_secs();
        band_stats st;
        build_band(&d, &bands[b], bo[b].cells, bo[b].nx, bo[b].ny, bo[b].tx0,
                   bo[b].ty0, &st);
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        deflateInit(&zs, 9);
        for (int64_t i = 0; i < (int64_t)bo[b].nx * bo[b].ny; i++) {
            cell_build *c = &bo[b].cells[i];
            if (c->n_recs == 0)
                continue;
            cell_sort(c);
            cell_compress(c, &zs);
            bo[b].payload_bytes += c->data.len;
            bo[b].present++;
        }
        deflateEnd(&zs);
        logi("[band %s] ways %lld  points %lld -> %lld  records %lld  "
             "cells %u/%d  payload %.2f MB  (%.1fs)\n",
             bands[b].name, (long long)st.ways_in, (long long)st.pts_in,
             (long long)st.pts_out, (long long)st.recs, bo[b].present,
             bo[b].nx * bo[b].ny, (double)bo[b].payload_bytes / 1048576.0,
             now_secs() - tb);
    }
    double t_geom = now_secs();

    /* ---- POI grid ------------------------------------------------------ */
    /* POIs feed the on-device labels: only the kinds the shared style can
     * ever label are kept (mm_style_label_*), the rest is dead weight in the
     * container and in every label scan. */
    int32_t *poi_remap = (int32_t *)malloc(sizeof(int32_t) * (size_t)(d.n_places ? d.n_places : 1));
    uint32_t *poi_new_off = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(d.n_places ? d.n_places : 1));
    if (!poi_remap || !poi_new_off) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    uint32_t poi_kept = 0;
    size_t new_strings_len = 0;
    for (int32_t i = 0; i < d.n_places; i++) {
        if (d.place_kind[i] > MM_PLACE_NEIGHBOURHOOD) {
            poi_remap[i] = -1;
            continue;
        }
        poi_remap[i] = (int32_t)poi_kept++;
        poi_new_off[i] = (uint32_t)new_strings_len;
        new_strings_len += d.name_len[i];
    }
    uint8_t *new_names = (uint8_t *)malloc(new_strings_len ? new_strings_len : 1);
    if (!new_names) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    {
        size_t pos = 0;
        for (int32_t i = 0; i < d.n_places; i++) {
            if (poi_remap[i] < 0)
                continue;
            memcpy(new_names + pos, d.names + d.name_off[i], d.name_len[i]);
            pos += d.name_len[i];
        }
    }
    logi("[poi] kept %u of %d places (others are never labeled)\n", poi_kept,
         d.n_places);

    double pn2 = pow(2.0, POIGRID_Z);
    int ptx0 = (int)floor(mercator_x(d.bbox[1], pn2) / 256.0);
    int ptx1 = (int)floor(mercator_x(d.bbox[3], pn2) / 256.0);
    int pty0 = (int)floor(mercator_y(d.bbox[2], pn2) / 256.0);
    int pty1 = (int)floor(mercator_y(d.bbox[0], pn2) / 256.0);
    int32_t pnx = ptx1 - ptx0 + 1, pny = pty1 - pty0 + 1;
    if (pnx <= 0 || pny <= 0 || (int64_t)pnx * pny > (1 << 22)) {
        fprintf(stderr, "bad POI grid\n");
        return 1;
    }
    poi_ref **pgrid = (poi_ref **)calloc((size_t)pnx * pny, sizeof(poi_ref *));
    uint32_t *pcounts = (uint32_t *)calloc((size_t)pnx * pny, 4);
    uint32_t *pcaps = (uint32_t *)calloc((size_t)pnx * pny, 4);
    uint32_t *pfirst = (uint32_t *)calloc((size_t)pnx * pny, 4);
    if (!pgrid || !pcounts || !pcaps || !pfirst) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (int32_t i = 0; i < d.n_places; i++) {
        if (poi_remap[i] < 0)
            continue;
        double x = mercator_x(d.place_lon_e5[i] / MM_E5, pn2);
        double y = mercator_y(d.place_lat_e5[i] / MM_E5, pn2);
        int cx = (int)floor(x / 256.0) - ptx0;
        int cy = (int)floor(y / 256.0) - pty0;
        if (cx < 0 || cy < 0 || cx >= pnx || cy >= pny)
            continue;
        size_t cell = (size_t)cy * pnx + cx;
        if (pcounts[cell] == pcaps[cell]) {
            pcaps[cell] = pcaps[cell] ? pcaps[cell] * 2 : 8;
            pgrid[cell] = (poi_ref *)realloc(pgrid[cell], sizeof(poi_ref) * pcaps[cell]);
            if (!pgrid[cell]) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
        }
        poi_ref *r = &pgrid[cell][pcounts[cell]++];
        r->poi = (uint32_t)poi_remap[i];
        r->score = mm_style_label_score(d.place_kind[i], d.place_population[i]);
    }
    uint32_t pg_present = 0, pg_refs = 0;
    for (int64_t i = 0; i < (int64_t)pnx * pny; i++) {
        if (pcounts[i] == 0)
            continue;
        qsort(pgrid[i], pcounts[i], sizeof(poi_ref), cmp_poi_ref);
        pfirst[i] = pg_refs;
        pg_refs += pcounts[i];
        pg_present++;
    }
    logi("[poi] grid z%d %dx%d  cells %u  refs %u\n", POIGRID_Z, pnx, pny,
         pg_present, pg_refs);

    /* ---- layout (pre-flight) ------------------------------------------- */
    uint16_t n_sections = 6; /* 3 bands + poigrid + poi + strings */
    uint64_t cursor = 128 + (uint64_t)n_sections * 16;

    typedef struct {
        uint64_t sec_off, presence_rel, prefix_rel, index_rel;
        uint64_t presence_bytes, prefix_bytes, index_bytes, payload_off;
    } layout;
    layout lay[3];
    for (int b = 0; b < 3; b++) {
        uint64_t cells = (uint64_t)bo[b].nx * bo[b].ny;
        lay[b].presence_bytes = (cells + 7) / 8;
        lay[b].prefix_bytes = ((uint64_t)bo[b].ny + 1) * 4;
        lay[b].index_bytes = (uint64_t)bo[b].present * MM_CHUNK_ENTRY_SIZE;
        lay[b].sec_off = cursor;
        cursor += MM_VBAND_SIZE;
        lay[b].presence_rel = MM_VBAND_SIZE;
        cursor += lay[b].presence_bytes;
        lay[b].prefix_rel = cursor - lay[b].sec_off;
        cursor += lay[b].prefix_bytes;
        lay[b].index_rel = cursor - lay[b].sec_off;
        cursor += lay[b].index_bytes;
    }
    uint64_t pg_presence_bytes = ((uint64_t)pnx * pny + 7) / 8;
    uint64_t pg_prefix_bytes = ((uint64_t)pny + 1) * 4;
    uint64_t pg_index_bytes = (uint64_t)pg_present * MM_POI_CELL_ENTRY_SIZE;
    uint64_t pg_refs_bytes = (uint64_t)pg_refs * MM_POI_REF_SIZE;
    uint64_t pg_sec_off = cursor;
    cursor += MM_POIGRID_SIZE;
    uint64_t pg_presence_rel = MM_POIGRID_SIZE;
    cursor += pg_presence_bytes;
    uint64_t pg_prefix_rel = cursor - pg_sec_off;
    cursor += pg_prefix_bytes;
    uint64_t pg_index_rel = cursor - pg_sec_off;
    cursor += pg_index_bytes;
    uint64_t pg_refs_rel = cursor - pg_sec_off;
    cursor += pg_refs_bytes;

    uint64_t poi_sec_off = cursor;
    cursor += 4 + (uint64_t)poi_kept * 20;
    uint64_t strings_sec_off = cursor;
    cursor += new_strings_len;

    cursor = (cursor + 3) & ~(uint64_t)3;
    for (int b = 0; b < 3; b++) {
        lay[b].payload_off = cursor;
        cursor += bo[b].payload_bytes;
    }
    uint64_t file_size = cursor;
    logi("[plan] file size    : %.2f MB (headers %.2f MB, payloads %.2f MB)\n",
         (double)file_size / 1048576.0,
         (double)lay[0].payload_off / 1048576.0,
         (double)(file_size - lay[0].payload_off) / 1048576.0);

    /* ---- assemble in memory -------------------------------------------- */
    uint8_t *file = (uint8_t *)calloc(1, (size_t)file_size);
    if (!file) {
        fprintf(stderr, "out of memory (%.0f MB)\n", (double)file_size / 1048576.0);
        return 1;
    }

    /* header */
    memcpy(file, MM_VMAP_MAGIC, 8);
    put_u16(file + 8, MM_VMAP_VERSION);
    put_u16(file + 10, 128);
    put_u32(file + 12, MM_FLAG_DEFLATE | MM_FLAG_POI | MM_FLAG_VECTOR |
                           ((uint32_t)MM_STYLE_VERSION << 16));
    put_u32(file + 16, 256);
    file[20] = 1;
    file[21] = 0;
    put_u16(file + 22, n_sections);
    put_u32(file + 24, 128);
    put_u32(file + 28, (uint32_t)file_size);
    for (int i = 0; i < 4; i++)
        put_i32(file + 32 + i * 4, (int32_t)llround(d.bbox[i] * 1e7));
    {
        char buf[MM_VMAP_NAME_LEN];
        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf), "%s", name);
        memcpy(file + 48, buf, MM_VMAP_NAME_LEN);
    }
    {
        char buf[MM_VMAP_ATTRIB_LEN];
        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf), "%s", "Map data (c) OpenStreetMap (ODbL)");
        memcpy(file + 80, buf, MM_VMAP_ATTRIB_LEN);
    }

    /* section table */
    uint64_t table = 128;
    for (int b = 0; b < 3; b++) {
        uint8_t *e = file + table + (uint64_t)b * 16;
        put_u32(e, MM_SEC_VBAND);
        put_u32(e + 4, (uint32_t)lay[b].sec_off);
        put_u32(e + 8, (uint32_t)(lay[b].index_rel + lay[b].index_bytes));
        put_u32(e + 12, (uint32_t)b);
    }
    {
        uint8_t *e = file + table + 3 * 16;
        put_u32(e, MM_SEC_POIGRID);
        put_u32(e + 4, (uint32_t)pg_sec_off);
        put_u32(e + 8, (uint32_t)(pg_refs_rel + pg_refs_bytes));
        put_u32(e + 12, 0);
    }
    {
        uint8_t *e = file + table + 4 * 16;
        put_u32(e, MM_SEC_POI);
        put_u32(e + 4, (uint32_t)poi_sec_off);
        put_u32(e + 8, (uint32_t)(4 + (uint64_t)poi_kept * 20));
        put_u32(e + 12, (uint32_t)d.n_places);
    }
    {
        uint8_t *e = file + table + 5 * 16;
        put_u32(e, MM_SEC_STRINGS);
        put_u32(e + 4, (uint32_t)strings_sec_off);
        put_u32(e + 8, (uint32_t)new_strings_len);
        put_u32(e + 12, 0);
    }

    /* band sections */
    for (int b = 0; b < 3; b++) {
        uint8_t *sec = file + lay[b].sec_off;
        sec[0] = (uint8_t)b;
        sec[1] = (uint8_t)bands[b].grid_z;
        put_u16(sec + 2, 0);
        put_u32(sec + 4, (uint32_t)bo[b].tx0);
        put_u32(sec + 8, (uint32_t)bo[b].nx);
        put_u32(sec + 12, (uint32_t)bo[b].ty0);
        put_u32(sec + 16, (uint32_t)bo[b].ny);
        put_u32(sec + 20, (uint32_t)lay[b].presence_rel);
        put_u32(sec + 24, (uint32_t)lay[b].prefix_rel);
        put_u32(sec + 28, (uint32_t)lay[b].index_rel);
        put_u32(sec + 32, (uint32_t)lay[b].payload_off);
        put_u32(sec + 36, (uint32_t)bo[b].payload_bytes);
        put_u32(sec + 40, bo[b].present);
        sec[44] = (uint8_t)bands[b].zmin;
        sec[45] = (uint8_t)bands[b].zmax;
        put_u16(sec + 46, (uint16_t)lround(bands[b].tol_px_zmax * 16));

        uint8_t *presence = sec + lay[b].presence_rel;
        uint8_t *prefix = sec + lay[b].prefix_rel;
        uint8_t *index = sec + lay[b].index_rel;
        uint64_t payload_cur = lay[b].payload_off;
        uint32_t rank = 0;
        for (int32_t y = 0; y < bo[b].ny; y++) {
            put_u32(prefix + (uint64_t)y * 4, rank);
            for (int32_t x = 0; x < bo[b].nx; x++) {
                cell_build *c = &bo[b].cells[(size_t)y * bo[b].nx + x];
                if (c->n_recs == 0)
                    continue;
                uint64_t bit = (uint64_t)y * bo[b].nx + x;
                presence[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
                uint8_t *e = index + (uint64_t)rank * MM_CHUNK_ENTRY_SIZE;
                put_u32(e, (uint32_t)payload_cur);
                put_u32(e + 4, (uint32_t)c->data.len);
                put_u16(e + 8, c->chunk_flags);
                put_u16(e + 10, 0);
                memcpy(file + payload_cur, c->data.buf, c->data.len);
                payload_cur += c->data.len;
                rank++;
            }
        }
        put_u32(prefix + (uint64_t)bo[b].ny * 4, rank);
    }

    /* POI grid section */
    {
        uint8_t *sec = file + pg_sec_off;
        sec[0] = (uint8_t)POIGRID_Z;
        put_u16(sec + 2, 0);
        put_u32(sec + 4, (uint32_t)ptx0);
        put_u32(sec + 8, (uint32_t)pnx);
        put_u32(sec + 12, (uint32_t)pty0);
        put_u32(sec + 16, (uint32_t)pny);
        put_u32(sec + 20, (uint32_t)pg_presence_rel);
        put_u32(sec + 24, (uint32_t)pg_prefix_rel);
        put_u32(sec + 28, (uint32_t)pg_index_rel);
        put_u32(sec + 32, (uint32_t)pg_refs_rel);
        put_u32(sec + 36, (uint32_t)pg_refs_bytes);
        put_u32(sec + 40, pg_present);
        put_u32(sec + 44, pg_refs);

        uint8_t *presence = sec + pg_presence_rel;
        uint8_t *prefix = sec + pg_prefix_rel;
        uint8_t *index = sec + pg_index_rel;
        uint8_t *refs = sec + pg_refs_rel;
        uint32_t rank = 0;
        for (int32_t y = 0; y < pny; y++) {
            put_u32(prefix + (uint64_t)y * 4, rank);
            for (int32_t x = 0; x < pnx; x++) {
                size_t cell = (size_t)y * pnx + x;
                if (pcounts[cell] == 0)
                    continue;
                uint64_t bit = cell;
                presence[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
                uint8_t *e = index + (uint64_t)rank * MM_POI_CELL_ENTRY_SIZE;
                put_u32(e, pfirst[cell]);
                put_u32(e + 4, pcounts[cell]);
                put_u32(e + 8, 0);
                for (uint32_t i = 0; i < pcounts[cell]; i++) {
                    uint8_t *r = refs + ((uint64_t)pfirst[cell] + i) * MM_POI_REF_SIZE;
                    put_u32(r, pgrid[cell][i].poi);
                    put_i32(r + 4, pgrid[cell][i].score);
                }
                rank++;
            }
        }
        put_u32(prefix + (uint64_t)pny * 4, rank);
    }

    /* POI + strings (only the kept, labelable places; names are repacked) */
    {
        uint8_t *sec = file + poi_sec_off;
        put_u32(sec, poi_kept);
        uint32_t slot = 0;
        for (int32_t i = 0; i < d.n_places; i++) {
            if (poi_remap[i] < 0)
                continue;
            uint8_t *e = sec + 4 + (uint64_t)slot * 20;
            put_i32(e, (int32_t)d.place_lat_e5[i] * 100);     /* e5 -> e7 */
            put_i32(e + 4, (int32_t)d.place_lon_e5[i] * 100);
            e[8] = d.place_kind[i];
            e[9] = 0;
            put_u16(e + 10, d.name_len[i]);
            put_i32(e + 12, d.place_population[i]);
            put_u32(e + 16, poi_new_off[i]);
            slot++;
        }
        memcpy(file + strings_sec_off, new_names, new_strings_len);
        free(new_names);
        free(poi_remap);
        free(poi_new_off);
    }

    double t_write = now_secs();
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    size_t wrote = fwrite(file, 1, (size_t)file_size, f);
    fclose(f);
    free(file);
    if (wrote != (size_t)file_size) {
        fprintf(stderr, "short write\n");
        return 1;
    }
    logi("[done] wrote %s (%.2f MB)  load %.1fs  geometry %.1fs  assemble+write %.1fs  "
         "total %.1fs\n",
         out_path, (double)file_size / 1048576.0, t_load - t_all, t_geom - t_load,
         now_secs() - t_write, now_secs() - t_all);
    mm_data_free(&d);
    return 0;
}

/* --------------------------------------------------------------- verify -- */

typedef struct {
    FILE *f;
} file_io_ctx;

static int64_t file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    file_io_ctx *c = (file_io_ctx *)ctx;
    if (fseeko(c->f, (off_t)off, SEEK_SET) != 0)
        return -1;
    return (int64_t)fread(buf, 1, len, c->f);
}

static int cmd_verify(const char *path, int deep)
{
    file_io_ctx ctx;
    ctx.f = fopen(path, "rb");
    if (!ctx.f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseeko(ctx.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(ctx.f);
    mm_vmap_io io = {file_read, &ctx, size};
    mm_vmap m;
    if (mm_vmap_open(&m, &io) != 0) {
        fprintf(stderr, "[FAIL] structure refused\n");
        fclose(ctx.f);
        return 1;
    }
    printf("verify: v%u style %u  bands %d  raster zooms %d  poi %u  name '%s'\n",
           m.version, m.style_version, m.n_bands, m.n_zooms, m.poi_count, m.name);

    uint64_t total_records = 0, total_points = 0, total_bytes = 0;
    for (int b = 0; b < m.n_bands; b++) {
        const mm_vmap_band *bd = &m.bands[b];
        uint64_t cells = (uint64_t)bd->nx * bd->ny;
        uint64_t present = 0;
        for (uint32_t cy = 0; cy < bd->ny && (present < bd->present_count || !deep); cy++) {
            for (uint32_t cx = 0; cx < bd->nx; cx++) {
                mm_vmap_cell cell;
                int rc = mm_vmap_cell_info(&m, b, bd->x0 + cx, bd->y0 + cy, &cell);
                if (rc < 0) {
                    fprintf(stderr, "[FAIL] band %d cell %u,%u lookup\n", b, cx, cy);
                    fclose(ctx.f);
                    return 1;
                }
                if (rc > 0)
                    continue;
                present++;
                total_bytes += cell.length;
                if (!deep)
                    continue;
                static uint8_t buf[1 << 20];
                size_t len = 0;
                uint16_t cflags = 0;
                if (mm_vmap_read_chunk(&m, b, bd->x0 + cx, bd->y0 + cy, buf,
                                       sizeof(buf), &len, &cflags) != 0) {
                    fprintf(stderr, "[FAIL] band %d cell %u,%u inflate\n", b, cx, cy);
                    fclose(ctx.f);
                    return 1;
                }
                mm_chunk_reader r;
                mm_chunk_reader_init(&r, buf, len);
                for (;;) {
                    int rr = mm_chunk_next_record(&r);
                    if (rr < 0) {
                        fprintf(stderr, "[FAIL] band %d cell %u,%u record decode\n", b, cx, cy);
                        fclose(ctx.f);
                        return 1;
                    }
                    if (rr > 0)
                        break;
                    total_records++;
                    uint32_t n = 1;
                    while (mm_chunk_next_point(&r) > 0)
                        n++;
                    total_points += n;
                }
            }
        }
        if (present != bd->present_count) {
            fprintf(stderr, "[FAIL] band %d present count mismatch (%u header)\n", b,
                    bd->present_count);
            fclose(ctx.f);
            return 1;
        }
        printf("  band %d: grid z%u %ux%u  present %u  payload %.2f MB\n", b,
               bd->grid_z, bd->nx, bd->ny, bd->present_count,
               (double)bd->payload_bytes / 1048576.0);
        (void)cells;
    }
    if (deep) {
        /* every POI must resolve and every grid ref must point at one */
        for (uint32_t i = 0; i < m.poi_count; i++) {
            mm_vmap_poi p;
            char name[128];
            if (mm_vmap_poi_get(&m, i, &p) != 0 ||
                mm_vmap_poi_name(&m, &p, name, sizeof(name)) < 0) {
                fprintf(stderr, "[FAIL] poi %u\n", i);
                fclose(ctx.f);
                return 1;
            }
        }
        printf("  deep: %llu records, %llu points decoded\n",
               (unsigned long long)total_records, (unsigned long long)total_points);
    }
    printf("  payloads %.2f MB\n", (double)total_bytes / 1048576.0);
    printf("[OK] structure and payloads are consistent\n");
    fclose(ctx.f);
    return 0;
}

static int cmd_info(const char *path)
{
    file_io_ctx ctx;
    ctx.f = fopen(path, "rb");
    if (!ctx.f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseeko(ctx.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(ctx.f);
    mm_vmap_io io = {file_read, &ctx, size};
    mm_vmap m;
    if (mm_vmap_open(&m, &io) != 0) {
        fprintf(stderr, "refused\n");
        fclose(ctx.f);
        return 1;
    }
    printf("file       : %s (%.2f MB)\n", path, (double)size / 1048576.0);
    printf("version    : %u  style %u  flags 0x%x\n", m.version, m.style_version,
           m.flags);
    printf("name       : %s\n", m.name);
    printf("attribution: %s\n", m.attribution);
    printf("bbox e7    : %d %d %d %d\n", m.bbox_e7[0], m.bbox_e7[1], m.bbox_e7[2],
           m.bbox_e7[3]);
    for (int b = 0; b < m.n_bands; b++) {
        const mm_vmap_band *bd = &m.bands[b];
        printf("band %d     : z%u-%u grid z%u %ux%u cells, %u present, %.2f MB payload\n",
               b, bd->zmin, bd->zmax, bd->grid_z, bd->nx, bd->ny, bd->present_count,
               (double)bd->payload_bytes / 1048576.0);
    }
    for (int i = 0; i < m.n_zooms; i++) {
        const mm_vmap_zoom *z = &m.zooms[i];
        printf("raster z%-2u : %ux%u cells, %u present, %.2f MB, %u bits\n", z->z,
               z->nx, z->ny, z->present_count, (double)z->payload_bytes / 1048576.0,
               z->bits);
    }
    printf("poi        : %u entries, strings %u bytes\n", m.poi_count,
           m.strings_bytes);
    if (m.has_poigrid)
        printf("poigrid    : z%u %ux%u cells, %u present, %u refs\n", m.poigrid_z,
               m.pg_nx, m.pg_ny, m.pg_present, m.pg_refs);
    fclose(ctx.f);
    return 0;
}

/* ------------------------------------------------------------------ main -- */

static void usage(void)
{
    fprintf(stderr,
            "monomesh-vmap - MonoMesh vector container (.mmap v2)\n"
            "  monomesh-vmap generate --cache A.mosmb --output B.mmap [--name NAME]\n"
            "  monomesh-vmap verify --input B.mmap [--deep]\n"
            "  monomesh-vmap info --input B.mmap\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    const char *cache = NULL, *out = NULL, *input = NULL, *name = "vector map";
    int deep = 0;
    double l_tol = BANDS[0].tol_px_zmax, m_tol = BANDS[1].tol_px_zmax,
           h_tol = BANDS[2].tol_px_zmax;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            cache = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)
            input = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "--l-tol") && i + 1 < argc)
            l_tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "--m-tol") && i + 1 < argc)
            m_tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "--h-tol") && i + 1 < argc)
            h_tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "--deep"))
            deep = 1;
        else if (!strcmp(argv[i], "--quiet"))
            g_quiet = 1;
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!strcmp(cmd, "generate") && cache && out)
        return generate(cache, out, name, l_tol, m_tol, h_tol);
    if (!strcmp(cmd, "verify") && input)
        return cmd_verify(input, deep);
    if (!strcmp(cmd, "info") && input)
        return cmd_info(input);
    usage();
    return 2;
}
