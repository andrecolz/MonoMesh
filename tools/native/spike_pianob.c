/* Phase 0 measurement spike for Piano B (vector container + on-device rasteriser).
 *
 * Standalone diagnostic tool: it reads a .mosmb cache (the same one the tile
 * generator uses), applies the candidate band class filters, clips the geometry
 * per candidate chunk grid, estimates the encoded chunk payload (header +
 * chunk-local fixed point + zigzag varint deltas), compresses it with zlib and
 * reports the numbers that decide chunk size, band count and encoding.
 *
 * It also times the existing render.c core on the busiest tiles as a host proxy
 * for the on-device frame budget and benchmarks zigzag varint decoding against
 * fixed int16 deltas.
 *
 * Usage:
 *   spike_pianob --cache file.mosmb --mode chunks --band H --zc 15 --tol 1.0
 *   spike_pianob --cache file.mosmb --mode render
 *   spike_pianob --cache file.mosmb --mode varint
 *
 * This is measurement code, not production code: it uses double precision
 * freely. The production core in libmapcore/ is fixed point.
 */
#include "monomesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------ args -- */

typedef struct {
    const char *cache;
    const char *mode;
    const char *band;
    int zc;          /* chunk grid zoom */
    double tol;      /* Douglas-Peucker tolerance in px at the band top zoom */
    int with_secondary; /* band L variant: keep secondary roads */
    int top;         /* report detail on the N worst chunks */
} opts;

/* --------------------------------------------------------------- bands --- */

/* Band definitions under test (see plan D1). The filter is the union of the
 * classes the style can draw inside the band's zoom range. */
typedef struct {
    const char *name;
    int zmin, zmax;     /* served zoom range */
    int road_min_prio;  /* keep highways with prio >= this (ROAD_ORDER index) */
    int keep_rail;
    int keep_stream;    /* stream/ditch/drain water lines */
    int keep_area;      /* water areas (non ring) */
    int keep_river;     /* river/canal lines */
} band_def;

/* ROAD_ORDER priority indexes (common.c): pedestrian 0 .. motorway_link 16 */
enum {
    PR_PEDESTRIAN = 0, PR_PATH = 1, PR_TRACK = 2, PR_SERVICE = 3,
    PR_LIVING = 4, PR_RESIDENTIAL = 5, PR_UNCLASSIFIED = 6,
    PR_TERTIARY = 7, PR_TERTIARY_LINK = 8, PR_SECONDARY = 9,
    PR_SECONDARY_LINK = 10, PR_PRIMARY = 11, PR_PRIMARY_LINK = 12,
    PR_TRUNK = 13, PR_TRUNK_LINK = 14, PR_MOTORWAY = 15, PR_MOTORWAY_LINK = 16
};

static const band_def BANDS[] = {
    /* L: motorway/trunk/primary per D1 (secondary kept only in the variant) */
    {"L", 7, 11, PR_PRIMARY, 1, 0, 1, 1},
    {"M", 12, 14, PR_SECONDARY, 1, 1, 1, 1},
    {"H", 15, 18, PR_PEDESTRIAN, 1, 1, 1, 1},
};

static const band_def *find_band(const char *name)
{
    for (size_t i = 0; i < sizeof(BANDS) / sizeof(BANDS[0]); i++)
        if (strcmp(BANDS[i].name, name) == 0)
            return &BANDS[i];
    return NULL;
}

static int way_in_band(const band_def *b, const mm_way *w, int with_secondary)
{
    if (w->type == MM_HIGHWAY) {
        int min = b->road_min_prio;
        if (with_secondary && b == &BANDS[0])
            min = PR_SECONDARY;
        return w->prio >= min;
    }
    if (w->type == MM_WATER) {
        switch (w->prio) {
        case MM_WATER_STREAM: return b->keep_stream;
        case MM_WATER_RIVER: return b->keep_river;
        case MM_WATER_AREA: return b->keep_area;
        case MM_WATER_RING_OUTER:
        case MM_WATER_RING_INNER: return 1;
        }
        return 0;
    }
    return b->keep_rail;
}

/* ------------------------------------------------------- simplification -- */

typedef struct {
    double x, y;
} pt2;

/* Recursive Douglas-Peucker on a flag array (host spike only). */
static void dp_mark(const pt2 *p, int a, int b, double tol2, unsigned char *keep)
{
    if (b <= a + 1)
        return;
    double dx = p[b].x - p[a].x, dy = p[b].y - p[a].y;
    double len2 = dx * dx + dy * dy;
    double best = -1.0;
    int besti = -1;
    for (int i = a + 1; i < b; i++) {
        double ex = p[i].x - p[a].x, ey = p[i].y - p[a].y;
        double d;
        if (len2 <= 0.0) {
            d = ex * ex + ey * ey;
        } else {
            double t = (ex * dx + ey * dy) / len2;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            double px = p[a].x + t * dx - p[i].x, py = p[a].y + t * dy - p[i].y;
            d = px * px + py * py;
        }
        if (d > best) {
            best = d;
            besti = i;
        }
    }
    if (best > tol2 && besti > 0) {
        keep[besti] = 1;
        dp_mark(p, a, besti, tol2, keep);
        dp_mark(p, besti, b, tol2, keep);
    }
}

/* Keeps the endpoints of every run: closed rings keep their start point. */
static int dp_simplify(const pt2 *in, int n, double tol, pt2 *out)
{
    if (n < 3) {
        for (int i = 0; i < n; i++)
            out[i] = in[i];
        return n;
    }
    unsigned char *keep = calloc((size_t)n, 1);
    if (!keep)
        return n;
    keep[0] = keep[n - 1] = 1;
    dp_mark(in, 0, n - 1, tol * tol, keep);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (keep[i])
            out[m++] = in[i];
    free(keep);
    return m;
}

/* -------------------------------------------------------------- clipping -- */

/* Liang-Barsky clip of one segment against an axis aligned rect. */
static int lb_clip(double *x0, double *y0, double *x1, double *y1,
                   double rx0, double ry0, double rx1, double ry1)
{
    double t0 = 0.0, t1 = 1.0;
    double dx = *x1 - *x0, dy = *y1 - *y0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {*x0 - rx0, rx1 - *x0, *y0 - ry0, ry1 - *y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0)
                return 0;
            continue;
        }
        double r = q[i] / p[i];
        if (p[i] < 0.0) {
            if (r > t1) return 0;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return 0;
            if (r < t1) t1 = r;
        }
    }
    double nx0 = *x0 + t0 * dx, ny0 = *y0 + t0 * dy;
    double nx1 = *x0 + t1 * dx, ny1 = *y0 + t1 * dy;
    *x0 = nx0;
    *y0 = ny0;
    *x1 = nx1;
    *y1 = ny1;
    return 1;
}

/* Clips a polyline to a rect, emitting runs of consecutive inside points into
 * out/outn (max_runs runs). Returns the number of emitted runs. */
static int clip_runs(const pt2 *in, int n, double rx0, double ry0, double rx1,
                     double ry1, pt2 *out, int *outn, int max_runs, int *npts_total)
{
    int runs = 0, cnt = 0;
    int open = 0;
    double px = 0, py = 0;
    for (int i = 0; i + 1 < n; i++) {
        double x0 = in[i].x, y0 = in[i].y, x1 = in[i + 1].x, y1 = in[i + 1].y;
        if (!lb_clip(&x0, &y0, &x1, &y1, rx0, ry0, rx1, ry1))
            continue;
        int continues = open && px == x0 && py == y0;
        if (!continues) {
            if (runs >= max_runs) {
                open = 0;
                continue;
            }
            if (runs > 0)
                outn[runs - 1] = cnt;
            runs++;
            cnt = 0;
            out[(runs - 1) * 4096 + cnt++] = (pt2){x0, y0};
        }
        if (runs > 0 && cnt < 4096)
            out[(runs - 1) * 4096 + cnt++] = (pt2){x1, y1};
        px = x1;
        py = y1;
        open = 1;
    }
    if (runs > 0)
        outn[runs - 1] = cnt;
    *npts_total = 0;
    for (int r = 0; r < runs; r++)
        *npts_total += outn[r];
    return runs;
}

/* ------------------------------------------------------------ varint ------ */

static int varint_put(unsigned char *dst, uint32_t v)
{
    int n = 0;
    while (v >= 0x80) {
        dst[n++] = (unsigned char)(v | 0x80);
        v >>= 7;
    }
    dst[n++] = (unsigned char)v;
    return n;
}

static int varint_get(const unsigned char *src, uint32_t *out)
{
    uint32_t v = 0;
    int n = 0;
    for (;;) {
        unsigned char b = src[n];
        v |= (uint32_t)(b & 0x7f) << (7 * n);
        n++;
        if (!(b & 0x80) || n >= 5)
            break;
    }
    *out = v;
    return n;
}

static uint32_t zigzag(int32_t v)
{
    return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

/* ---------------------------------------------------------- chunk stats --- */

typedef struct {
    unsigned char *buf;
    size_t len, cap;
    int32_t n_ways, n_points, n_runs;
} chunk_buf;

static void cb_put(chunk_buf *c, const unsigned char *data, size_t n)
{
    if (c->len + n > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 4096;
        while (cap < c->len + n)
            cap *= 2;
        c->buf = realloc(c->buf, cap);
        if (!c->buf) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        c->cap = cap;
    }
    memcpy(c->buf + c->len, data, n);
    c->len += n;
}

static double *project(const mm_data *d, int z)
{
    double n = pow(2.0, z);
    double *xy = malloc(sizeof(double) * 2 * (size_t)(d->n_points ? d->n_points : 1));
    if (!xy) {
        fprintf(stderr, "out of memory (projection)\n");
        exit(1);
    }
    for (int32_t i = 0; i < d->n_points; i++) {
        double lat = d->lat_e5[i] / MM_E5, lon = d->lon_e5[i] / MM_E5;
        xy[2 * i] = (lon + 180.0) / 360.0 * n * 256.0;  /* world pixels at z */
        xy[2 * i + 1] = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * n * 256.0;
    }
    return xy;
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? 1 : (x > y ? -1 : 0);
}

/* World pixel -> tile index helpers for the per-screen accounting. */
typedef struct {
    int64_t idx;
    size_t size;
} cell_ref;

static int cmp_cell_ref(const void *a, const void *b)
{
    const cell_ref *x = a, *y = b;
    return x->size < y->size ? 1 : (x->size > y->size ? -1 : 0);
}

static void bbox_of_cell(int zc, int tx, int ty, double *lat0, double *lat1,
                         double *lon0, double *lon1)
{
    double n2 = pow(2.0, zc);
    *lat0 = atan(sinh(M_PI * (1.0 - 2.0 * ((double)ty + 1) / n2))) * 180.0 / M_PI;
    *lat1 = atan(sinh(M_PI * (1.0 - 2.0 * ((double)ty) / n2))) * 180.0 / M_PI;
    *lon0 = ((double)tx / n2) * 360.0 - 180.0;
    *lon1 = ((double)(tx + 1) / n2) * 360.0 - 180.0;
}

/* Non-empty cells and bytes inside a 480x583 screen centred on lat/lon at zoom z. */
static void screen_stats(const chunk_buf *cells, int nx, int ny, int tx0, int ty0,
                         int zc, int z, double clat, double clon, const char *label)
{
    double n2 = pow(2.0, zc);
    double cell_px = 256.0 * pow(2.0, z - zc);
    double ctx_f = (clon + 180.0) / 360.0 * n2 * 256.0;
    double cty_f = (1.0 - asinh(tan(clat * M_PI / 180.0)) / M_PI) / 2.0 * n2 * 256.0;
    /* screen half extents in world px at this zoom */
    double hx = 480.0 / cell_px * 128.0, hy = 583.0 / cell_px * 128.0;
    int cx0 = (int)floor((ctx_f - hx) / 256.0) - tx0;
    int cx1 = (int)floor((ctx_f + hx) / 256.0) - tx0;
    int cy0 = (int)floor((cty_f - hy) / 256.0) - ty0;
    int cy1 = (int)floor((cty_f + hy) / 256.0) - ty0;
    int64_t n = 0, bytes = 0, total = 0;
    for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            if (cx < 0 || cy < 0 || cx >= nx || cy >= ny)
                continue;
            const chunk_buf *c = &cells[(size_t)cy * nx + cx];
            total++;
            if (c->len) {
                n++;
                bytes += (int64_t)c->len;
            }
        }
    }
    printf("  screen z%-2d @ %-7s: %lld cells touched, %lld non-empty, "
           "%.0f KB compressed payload\n",
           z, label, (long long)total, (long long)n, (double)bytes / 1024.0);
}

static int run_chunks(const opts *o, const mm_data *d)
{
    const band_def *b = find_band(o->band);
    if (!b) {
        fprintf(stderr, "unknown band %s\n", o->band);
        return 1;
    }
    int zc = o->zc;
    double tol = o->tol;

    /* Project once at the band top zoom: DP tolerance is px at zmax. */
    int zref = b->zmax;
    double *xy = project(d, zref);
    double scale_to_zc = 1.0 / (double)(1 << (zref > zc ? zref - zc : 1)) *
                         pow(2.0, zc - zref); /* world px at zc = world px at zref * 2^(zc-zref) */
    (void)scale_to_zc;
    double px_scale = pow(2.0, zc - zref); /* multiply zref px by this to get zc px */

    /* chunk grid extent over the data bbox */
    double n2 = pow(2.0, zc);
    double wx0 = (d->bbox[1] + 180.0) / 360.0 * n2 * 256.0;
    double wx1 = (d->bbox[3] + 180.0) / 360.0 * n2 * 256.0;
    double wy0 = (1.0 - asinh(tan(d->bbox[2] * M_PI / 180.0)) / M_PI) / 2.0 * n2 * 256.0;
    double wy1 = (1.0 - asinh(tan(d->bbox[0] * M_PI / 180.0)) / M_PI) / 2.0 * n2 * 256.0;
    int tx0 = (int)floor(wx0 / 256.0), tx1 = (int)floor(wx1 / 256.0);
    int ty0 = (int)floor(wy0 / 256.0), ty1 = (int)floor(wy1 / 256.0);
    int nx = tx1 - tx0 + 1, ny = ty1 - ty0 + 1;
    printf("band %s (z%d-%d) chunk grid z%d: %d x %d = %d cells, tol %.2f px @ z%d\n",
           b->name, b->zmin, b->zmax, zc, nx, ny, nx * ny, tol, zref);
    if ((int64_t)nx * ny > 2000000) {
        fprintf(stderr, "grid too large\n");
        return 1;
    }
    chunk_buf *cells = calloc((size_t)nx * ny, sizeof(chunk_buf));
    if (!cells) {
        fprintf(stderr, "out of memory (grid)\n");
        return 1;
    }

    pt2 *way_pts = malloc(sizeof(pt2) * (size_t)(d->n_points ? d->n_points : 1));
    pt2 *simp = malloc(sizeof(pt2) * (size_t)(d->n_points ? d->n_points : 1));
    pt2 *clipped = malloc(sizeof(pt2) * 4096 * 32);
    int run_lens[32];
    if (!way_pts || !simp || !clipped) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    int64_t kept_ways = 0, kept_pts = 0, simp_pts = 0, dup_pts = 0, enc_bytes = 0;
    double t0 = mm_now();
    for (int32_t wi = 0; wi < d->n_ways; wi++) {
        const mm_way *w = &d->ways[wi];
        if (!way_in_band(b, w, o->with_secondary))
            continue;
        if (w->npts < 2)
            continue;
        kept_ways++;
        kept_pts += w->npts;
        for (int32_t j = 0; j < w->npts; j++) {
            way_pts[j].x = xy[2 * (w->off + j)];
            way_pts[j].y = xy[2 * (w->off + j) + 1];
        }
        /* simplify in zref pixels */
        int n = dp_simplify(way_pts, w->npts, tol, simp);
        /* closed ways keep their ring closed */
        if (w->closed && n >= 3 && (simp[0].x != simp[n - 1].x || simp[0].y != simp[n - 1].y))
            simp[n++] = simp[0];
        simp_pts += n;

        /* way bbox in zc pixels */
        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for (int j = 0; j < n; j++) {
            double x = simp[j].x * px_scale, y = simp[j].y * px_scale;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
        int cx0 = (int)floor(minx / 256.0) - tx0, cx1 = (int)floor(maxx / 256.0) - tx0;
        int cy0 = (int)floor(miny / 256.0) - ty0, cy1 = (int)floor(maxy / 256.0) - ty0;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > nx - 1) cx1 = nx - 1;
        if (cy1 > ny - 1) cy1 = ny - 1;

        /* scale points to zc pixels for clipping */
        for (int j = 0; j < n; j++) {
            simp[j].x *= px_scale;
            simp[j].y *= px_scale;
        }
        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                double rx0 = (tx0 + cx) * 256.0, ry0 = (ty0 + cy) * 256.0;
                int total = 0;
                int runs = clip_runs(simp, n, rx0, ry0, rx0 + 256.0, ry0 + 256.0,
                                     clipped, run_lens, 32, &total);
                if (runs == 0 || total < 2)
                    continue;
                chunk_buf *c = &cells[(size_t)cy * nx + cx];
                unsigned char hdr[16];
                int hl = 0;
                hdr[hl++] = (unsigned char)(w->type << 5 | (w->prio & 0x1f));
                hdr[hl++] = (unsigned char)(w->closed ? 1 : 0);
                for (int r = 0; r < runs; r++) {
                    if (run_lens[r] < 2)
                        continue;
                    int k = hl;
                    hl += varint_put(hdr + hl, (uint32_t)run_lens[r]);
                    cb_put(c, hdr, (size_t)hl);
                    hl = k;
                    /* first point absolute chunk-local fixed point 1/16 px */
                    unsigned char pb[16];
                    int32_t fx = (int32_t)lround((clipped[r * 4096].x - rx0) * 16.0);
                    int32_t fy = (int32_t)lround((clipped[r * 4096].y - ry0) * 16.0);
                    memcpy(pb, &fx, 4);
                    memcpy(pb + 4, &fy, 4);
                    cb_put(c, pb, 8);
                    int32_t px = fx, py = fy;
                    for (int j = 1; j < run_lens[r]; j++) {
                        int32_t qx = (int32_t)lround((clipped[r * 4096 + j].x - rx0) * 16.0);
                        int32_t qy = (int32_t)lround((clipped[r * 4096 + j].y - ry0) * 16.0);
                        int off = 0;
                        off += varint_put(pb + off, zigzag(qx - px));
                        off += varint_put(pb + off, zigzag(qy - py));
                        cb_put(c, pb, (size_t)off);
                        px = qx;
                        py = qy;
                    }
                    c->n_points += run_lens[r];
                    c->n_runs++;
                    enc_bytes += 8 + 2 * run_lens[r];
                    dup_pts += run_lens[r];
                }
                c->n_ways++;
            }
        }
    }
    double t1 = mm_now();

    /* compress each non-empty cell (zlib 9, shared deflate stream) */
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    deflateInit(&zs, 9);
    int64_t comp_total = 0;
    int64_t nonempty = 0;
    int64_t comp_hist[8] = {0};
    double t2 = mm_now();
    for (int64_t i = 0; i < (int64_t)nx * ny; i++) {
        chunk_buf *c = &cells[i];
        if (!c->len)
            continue;
        nonempty++;
        uLongf bound = compressBound((uLong)c->len);
        unsigned char *zbuf = malloc(bound ? bound : 1);
        zs.next_in = c->buf;
        zs.avail_in = (uInt)c->len;
        zs.next_out = zbuf;
        zs.avail_out = (uInt)bound;
        if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
            fprintf(stderr, "deflate failed\n");
            return 1;
        }
        comp_total += (int64_t)zs.total_out;
        size_t zl = zs.total_out;
        int bucket = zl <= 4096 ? 0 : zl <= 8192 ? 1 : zl <= 16384 ? 2 : zl <= 32768 ? 3
                   : zl <= 65536 ? 4 : zl <= 131072 ? 5 : zl <= 262144 ? 6 : 7;
        comp_hist[bucket]++;
        free(zbuf);
        deflateReset(&zs);
    }
    deflateEnd(&zs);
    double t3 = mm_now();

    /* size distribution */
    size_t *sizes = malloc(sizeof(size_t) * (size_t)(nonempty ? nonempty : 1));
    cell_ref *refs = malloc(sizeof(cell_ref) * (size_t)(nonempty ? nonempty : 1));
    int64_t k = 0;
    for (int64_t i = 0; i < (int64_t)nx * ny; i++) {
        if (!cells[i].len)
            continue;
        sizes[k] = cells[i].len;
        refs[k].idx = i;
        refs[k].size = cells[i].len;
        k++;
    }
    qsort(sizes, (size_t)k, sizeof(size_t), cmp_size);         /* descending */
    qsort(refs, (size_t)k, sizeof(cell_ref), cmp_cell_ref);    /* descending */
    size_t p50 = k ? sizes[k / 2] : 0, p95 = k ? sizes[(size_t)(k * 0.05)] : 0;
    size_t mx = k ? sizes[0] : 0;

    printf("  ways kept %lld  points %lld -> simplified %lld (%.0f%%)  duplication %.2fx\n",
           (long long)kept_ways, (long long)kept_pts, (long long)simp_pts,
           kept_pts ? 100.0 * simp_pts / kept_pts : 0.0,
           simp_pts ? (double)dup_pts / simp_pts : 0.0);
    printf("  nonempty cells %lld / %d  decompressed total %.2f MB  (rough raw est %.2f MB)\n",
           (long long)nonempty, nx * ny, (double)enc_bytes / 1048576.0,
           (double)enc_bytes / 1048576.0);
    printf("  cell decompressed: p50 %zu B  p95 %zu B  max %zu B  avg %lld B\n",
           p50, p95, mx, nonempty ? (long long)(enc_bytes / nonempty) : 0);
    printf("  compressed (zlib9): %.2f MB total, ratio %.2f, avg %lld B/cell\n",
           (double)comp_total / 1048576.0,
           comp_total ? (double)enc_bytes / comp_total : 0.0,
           nonempty ? (long long)(comp_total / nonempty) : 0);
    printf("  compressed cell histogram: <=4K %lld  <=8K %lld  <=16K %lld  <=32K %lld"
           "  <=64K %lld  <=128K %lld  <=256K %lld  >256K %lld\n",
           (long long)comp_hist[0], (long long)comp_hist[1], (long long)comp_hist[2],
           (long long)comp_hist[3], (long long)comp_hist[4], (long long)comp_hist[5],
           (long long)comp_hist[6], (long long)comp_hist[7]);
    printf("  time: clip+encode %.2fs  deflate %.2fs\n", t1 - t0, t3 - t2);

    /* screen chunk counts: how many cells a 480x583 screen needs */
    for (int z = b->zmin; z <= b->zmax; z++) {
        double cell_px = 256.0 * pow(2.0, z - zc);
        int sx = (int)ceil(480.0 / cell_px) + 1, sy = (int)ceil(583.0 / cell_px) + 1;
        printf("  at z%-2d a cell is %6.0f screen px -> %d x %d = %d cells/screen\n",
               z, cell_px, sx, sy, sx * sy);
    }
    /* real per-screen cost at every band zoom, on dense and rural windows */
    for (int z = b->zmin; z <= b->zmax; z++) {
        screen_stats(cells, nx, ny, tx0, ty0, zc, z, 45.4642, 9.19, "Milano");
        screen_stats(cells, nx, ny, tx0, ty0, zc, z, 45.10, 7.40, "Torino");
        screen_stats(cells, nx, ny, tx0, ty0, zc, z, 46.10, 8.30, "Alpe");
    }

    if (o->top > 0) {
        int top = o->top < (int)k ? o->top : (int)k;
        for (int i = 0; i < top; i++) {
            int cx = (int)(refs[i].idx % nx), cy = (int)(refs[i].idx / nx);
            double lat0, lat1, lon0, lon1;
            bbox_of_cell(zc, tx0 + cx, ty0 + cy, &lat0, &lat1, &lon0, &lon1);
            const chunk_buf *c = &cells[refs[i].idx];
            printf("  worst #%d cell (%d,%d) %zu B decompressed, %d ways, %d points: "
                   "bbox lat %.4f..%.4f lon %.4f..%.4f\n",
                   i + 1, cx, cy, refs[i].size, c->n_ways, c->n_points,
                   lat0, lat1, lon0, lon1);
        }
    }
    free(sizes);
    free(refs);
    return 0;
}

/* ---------------------------------------------------------- render proxy -- */

static int run_render(const opts *o, const mm_data *d)
{
    (void)o;
    int zs[2] = {14, 16};
    double *xy = mm_project_points(d, 14);
    if (!xy) {
        fprintf(stderr, "projection failed\n");
        return 1;
    }
    for (int zi = 0; zi < 2; zi++) {
        int z = zs[zi];
        mm_index *ix = mm_index_build(d, z, 14, xy);
        if (!ix) {
            fprintf(stderr, "index failed\n");
            return 1;
        }
        /* Sample the grid: every tile at z14, a stride at z16, and always the
         * dense Milan tiles (they are the design's worst case). */
        mm_grid g = mm_tile_grid(d, z);
        uint8_t *img = malloc((size_t)256 * 256);
        double worst_t = 0.0, total = 0.0;
        int worst_tx = 0, worst_ty = 0, rendered = 0;
        double n2 = pow(2.0, z);
        int mil_tx = (int)((9.19 + 180.0) / 360.0 * n2);
        int mil_ty = (int)((1.0 - asinh(tan(45.4642 * M_PI / 180.0)) / M_PI) / 2.0 * n2);
        for (int ty = 0; ty < g.ny; ty++) {
            for (int tx = 0; tx < g.nx; tx++) {
                int stride = (z >= 16) ? 24 : 1;
                int near_milan = abs(tx - mil_tx) <= 1 && abs(ty - mil_ty) <= 1;
                if (((tx * 7 + ty * 13) % stride) != 0 && !near_milan)
                    continue;
                double t0 = mm_now();
                mm_render_tile_img(ix, z, g.tx0 + tx, g.ty0 + ty, 0, img);
                double dt = mm_now() - t0;
                total += dt;
                rendered++;
                if (dt > worst_t) {
                    worst_t = dt;
                    worst_tx = g.tx0 + tx;
                    worst_ty = g.ty0 + ty;
                }
            }
        }
        printf("z%d: %d tiles sampled, avg %.2f ms, worst %.2f ms at tile %d,%d "
               "(host core, full ways per tile; one 480x583 screen ~ 2.3 tiles)\n",
               z, rendered, total / (rendered ? rendered : 1) * 1000.0, worst_t * 1000.0,
               worst_tx, worst_ty);
        free(img);
        mm_index_free(ix);
    }
    free(xy);
    return 0;
}

/* ------------------------------------------------------------- varint ----- */

static int run_varint(const opts *o, const mm_data *d)
{
    (void)o;
    (void)d;
    const int N = 4 * 1000 * 1000;
    int32_t *vals = malloc(sizeof(int32_t) * (size_t)N);
    unsigned char *enc = malloc((size_t)N * 4);
    if (!vals || !enc)
        return 1;
    int32_t v = 0;
    uint32_t seed = 12345;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245 + 12345;
        int32_t step = (int32_t)((seed >> 16) % 17) - 8; /* -8..8 like real deltas */
        v += step;
        vals[i] = v;
    }
    size_t enc_len = 0;
    int32_t prev = 0;
    for (int i = 0; i < N; i++) {
        enc_len += (size_t)varint_put(enc + enc_len, zigzag(vals[i] - prev));
        prev = vals[i];
    }

    /* zigzag varint decode */
    double t0 = mm_now();
    int32_t acc = 0, cur = 0;
    for (int rep = 0; rep < 20; rep++) {
        size_t pos = 0;
        cur = 0;
        for (int i = 0; i < N; i++) {
            uint32_t z;
            pos += (size_t)varint_get(enc + pos, &z);
            cur += (int32_t)((z >> 1) ^ -(int32_t)(z & 1));
            acc += cur;
        }
    }
    double t1 = mm_now();

    /* fixed int16 deltas */
    int16_t *d16 = malloc(sizeof(int16_t) * (size_t)N);
    prev = 0;
    for (int i = 0; i < N; i++) {
        d16[i] = (int16_t)(vals[i] - prev);
        prev = vals[i];
    }
    double t2 = mm_now();
    for (int rep = 0; rep < 20; rep++) {
        cur = 0;
        for (int i = 0; i < N; i++) {
            cur += d16[i];
            acc += cur;
        }
    }
    double t3 = mm_now();

    double mb = (double)enc_len / 1048576.0;
    printf("varint: %zu bytes for %d coords (%.2f B/coord)\n", enc_len, N * 2,
           (double)enc_len / (N * 2.0));
    printf("  zigzag-varint decode: %.1f MB/s (%.1f Mcodes/s)\n",
           mb * 20.0 / (t1 - t0), (double)N * 20.0 / (t1 - t0) / 1e6);
    printf("  int16 fixed delta:    %.1f MB/s (%.1f Mcodes/s)\n",
           (double)N * 2 * 20.0 / (t3 - t2) / 1048576.0, (double)N * 20.0 / (t3 - t2) / 1e6);
    printf("  acc=%d (work checksum)\n", acc == 12345 ? 1 : 0);
    return 0;
}

/* ----------------------------------------------------------------- main --- */

int main(int argc, char **argv)
{
    opts o = {.cache = NULL, .mode = "chunks", .band = "H", .zc = 15,
              .tol = 1.0, .with_secondary = 1, .top = 3};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            o.cache = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc)
            o.mode = argv[++i];
        else if (!strcmp(argv[i], "--band") && i + 1 < argc)
            o.band = argv[++i];
        else if (!strcmp(argv[i], "--zc") && i + 1 < argc)
            o.zc = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tol") && i + 1 < argc)
            o.tol = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-secondary"))
            o.with_secondary = 0;
        else {
            fprintf(stderr, "unknown arg %s\n", argv[i]);
            return 2;
        }
    }
    if (!o.cache) {
        fprintf(stderr, "usage: spike_pianob --cache f.mosmb [--mode chunks|render|varint] "
                        "[--band L|M|H] [--zc N] [--tol px] [--no-secondary]\n");
        return 2;
    }
    mm_data d;
    if (mm_cache_read(o.cache, &d) != 0) {
        fprintf(stderr, "cannot read cache %s\n", o.cache);
        return 1;
    }
    printf("cache: %s  ways %d  points %d  places %d\n", o.cache, d.n_ways,
           d.n_points, d.n_places);
    int rc;
    if (!strcmp(o.mode, "chunks"))
        rc = run_chunks(&o, &d);
    else if (!strcmp(o.mode, "render"))
        rc = run_render(&o, &d);
    else if (!strcmp(o.mode, "varint"))
        rc = run_varint(&o, &d);
    else {
        fprintf(stderr, "unknown mode %s\n", o.mode);
        rc = 2;
    }
    mm_data_free(&d);
    return rc;
}
