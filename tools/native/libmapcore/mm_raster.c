#include "mm_raster.h"
#include "mm_geom.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- canvas ---- */

static inline int clip_x0(const mm_canvas *c) { return mm_px(c->clip.x0); }
static inline int clip_x1(const mm_canvas *c) { return mm_px(c->clip.x1); }
static inline int clip_y0(const mm_canvas *c) { return mm_px(c->clip.y0); }
static inline int clip_y1(const mm_canvas *c) { return mm_px(c->clip.y1); }

static inline void put_px(mm_canvas *c, int x, int y, uint8_t v)
{
    if (x < 0 || y < 0 || x >= c->width || y >= c->height)
        return;
    if (x < clip_x0(c) || x > clip_x1(c) || y < clip_y0(c) || y > clip_y1(c))
        return;
    c->pixels[(int32_t)y * c->stride + x] = v;
}

void mm_canvas_put(mm_canvas *c, int x, int y, uint8_t v)
{
    put_px(c, x, y, v);
}

void mm_canvas_clear(mm_canvas *c, uint8_t v)
{
    for (int32_t y = 0; y < c->height; y++)
        memset(c->pixels + (int32_t)y * c->stride, v, (size_t)c->width);
}

void mm_canvas_fill_rect(mm_canvas *c, mm_rect r, uint8_t v)
{
    int x0 = mm_px(r.x0), x1 = mm_px(r.x1), y0 = mm_px(r.y0), y1 = mm_px(r.y1);
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (x0 < clip_x0(c)) x0 = clip_x0(c);
    if (x1 > clip_x1(c)) x1 = clip_x1(c);
    if (y0 < clip_y0(c)) y0 = clip_y0(c);
    if (y1 > clip_y1(c)) y1 = clip_y1(c);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= c->height)
            continue;
        memset(c->pixels + (int32_t)y * c->stride + x0, v, (size_t)(x1 - x0 + 1));
    }
}

/* --------------------------------------------------------------- lines --- */

void mm_draw_line(mm_canvas *c, mm_pt a, mm_pt b, uint8_t v)
{
    int x0 = mm_px(a.x), y0 = mm_px(a.y), x1 = mm_px(b.x), y1 = mm_px(b.y);
    /* cheap rejection against the clip window */
    int cx0 = clip_x0(c), cx1 = clip_x1(c), cy0 = clip_y0(c), cy1 = clip_y1(c);
    if ((x0 < cx0 && x1 < cx0) || (x0 > cx1 && x1 > cx1) ||
        (y0 < cy0 && y1 < cy0) || (y0 > cy1 && y1 > cy1))
        return;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(c, x0, y0, v);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void mm_draw_disk(mm_canvas *c, mm_pt center, int32_t r, uint8_t v)
{
    if (r <= 0)
        return;
    int x0 = (int)((center.x - r + 255) >> 8); /* ceil */
    int x1 = (int)((center.x + r) >> 8);       /* floor */
    int y0 = (int)((center.y - r + 255) >> 8);
    int y1 = (int)((center.y + r) >> 8);
    int64_t r2 = (int64_t)r * r;
    for (int y = y0; y <= y1; y++) {
        int64_t dy = (int64_t)y * 256 - center.y;
        for (int x = x0; x <= x1; x++) {
            int64_t dx = (int64_t)x * 256 - center.x;
            if (dx * dx + dy * dy <= r2)
                put_px(c, x, y, v);
        }
    }
}

/* ------------------------------------------------------------- polygon --- */

typedef struct {
    int32_t ylo, yhi;   /* pixel rows this edge crosses (half open) */
    int32_t ax, ay, bx, by; /* endpoints in 24.8 (int32 is the format bound) */
} mm_edge;

static int cmp_edge(const void *pa, const void *pb)
{
    const mm_edge *a = (const mm_edge *)pa, *b = (const mm_edge *)pb;
    if (a->ylo != b->ylo)
        return a->ylo < b->ylo ? -1 : 1;
    return 0;
}

/* ceil(v/256) without shifting negative values (portable and deterministic
 * on every compiler: the host and the device must round identically). */
static inline int32_t ceil_px(int64_t v)
{
    return (int32_t)(v >= 0 ? (v + 255) / 256 : v / 256);
}

/* Workspace per point: edge + active index + crossing.  32 bytes/point, so a
 * 1200 point water ring fits the 64 KB canvas arena with room for the caller's
 * point buffer. */
#define MM_FILL_WS_PER_PT (sizeof(mm_edge) + sizeof(int) + sizeof(int64_t))

void mm_fill_polygon(mm_canvas *c, const mm_pt *pts, int n, uint8_t v)
{
    if (n < 3)
        return;
    size_t need = MM_FILL_WS_PER_PT * (size_t)n;
    /* the workspace must be 8 byte aligned whatever offset the caller hands us */
    uint8_t *ws = NULL;
    int heap = 0;
    if (c->scratch) {
        uintptr_t base = (uintptr_t)c->scratch;
        uintptr_t aligned = (base + 7) & ~(uintptr_t)7;
        if (c->scratch_len >= (aligned - base) + need)
            ws = (uint8_t *)aligned;
    }
    if (!ws) {
        ws = (uint8_t *)malloc(need);
        heap = 1;
    }
    if (!ws)
        return;
    int64_t *xs = (int64_t *)ws;
    int *active = (int *)(ws + sizeof(int64_t) * (size_t)n);
    mm_edge *edges = (mm_edge *)(ws + sizeof(int64_t) * (size_t)n + sizeof(int) * (size_t)n);

    int32_t ymin = INT32_MAX, ymax = INT32_MIN;
    int ne = 0;
    for (int i = 0; i < n; i++) {
        mm_pt p0 = pts[i], p1 = pts[(i + 1) % n];
        if (p0.y == p1.y)
            continue;
        mm_edge *e = &edges[ne];
        if (p0.y < p1.y) {
            e->ax = p0.x;
            e->ay = p0.y;
            e->bx = p1.x;
            e->by = p1.y;
        } else {
            e->ax = p1.x;
            e->ay = p1.y;
            e->bx = p0.x;
            e->by = p0.y;
        }
        /* rows crossed: a crossing at pixel centre y*256 satisfies ay <= yc < by */
        e->ylo = ceil_px(e->ay);
        e->yhi = ceil_px(e->by);
        if (e->yhi <= e->ylo)
            continue;
        if (e->ylo < ymin) ymin = e->ylo;
        if (e->yhi > ymax) ymax = e->yhi;
        ne++;
    }
    if (ne < 2) {
        if (heap)
            free(ws);
        return;
    }
    qsort(edges, (size_t)ne, sizeof(mm_edge), cmp_edge);

    int32_t y_from = ymin, y_to = ymax; /* [y_from, y_to) */
    if (y_from < clip_y0(c)) y_from = clip_y0(c);
    if (y_to > clip_y1(c) + 1) y_to = clip_y1(c) + 1;
    if (y_from < 0) y_from = 0;
    if (y_to > c->height) y_to = c->height;

    int next = 0;
    for (int32_t y = y_from; y < y_to; y++) {
        /* active edge table: edges are sorted by ylo, so we just advance */
        while (next < ne && edges[next].ylo <= y)
            next++;
        int na = 0;
        for (int i = 0; i < next; i++) {
            const mm_edge *e = &edges[i];
            if (e->yhi <= y)
                continue;
            /* exact crossing of the pixel-centre line (no incremental drift) */
            int64_t yc = (int64_t)y * 256;
            int64_t x = e->ax + (yc - e->ay) * (e->bx - e->ax) / (e->by - e->ay);
            active[na] = i;
            xs[na] = x;
            na++;
        }
        /* insertion sort by x (small n) */
        for (int i = 1; i < na; i++) {
            int64_t key = xs[i];
            int keye = active[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > key) {
                xs[j + 1] = xs[j];
                active[j + 1] = active[j];
                j--;
            }
            xs[j + 1] = key;
            active[j + 1] = keye;
        }
        for (int i = 0; i + 1 < na; i += 2) {
            int32_t a = ceil_px(xs[i]);
            int32_t b = ceil_px(xs[i + 1]) - 1;
            if (a < clip_x0(c)) a = clip_x0(c);
            if (b > clip_x1(c)) b = clip_x1(c);
            if (a < 0) a = 0;
            if (b > c->width - 1) b = c->width - 1;
            if (b < a)
                continue;
            memset(c->pixels + (int32_t)y * c->stride + a, v, (size_t)(b - a + 1));
        }
    }
    if (heap)
        free(ws);
}

/* ------------------------------------------------------------- strokes --- */

static int32_t isqrt64(int64_t v)
{
    if (v <= 0)
        return 0;
    int64_t x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return (int32_t)x;
}

/* Filled band of one segment: offset by +/- width/2 along the perpendicular.
 * The two strokes of a road (casing + fill) share the rounded endpoints, so
 * the bands always agree to the pixel. */
static void fill_band(mm_canvas *c, mm_pt p0, mm_pt p1, int32_t width, uint8_t v)
{
    int32_t dx = p1.x - p0.x, dy = p1.y - p0.y;
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
    int32_t len = isqrt64(d2);
    if (len < 256 / 8) { /* degenerate: a dot */
        mm_draw_disk(c, p0, width / 2, v);
        return;
    }
    /* unit perpendicular in 24.8 (multiplication, not shifts: -dy can be
     * negative and a left shift of a negative value is undefined behaviour -
     * found by the UBSan build of the test driver) */
    int32_t nx = (int32_t)(((int64_t)-dy * 256) / len);
    int32_t ny = (int32_t)(((int64_t)dx * 256) / len);
    int32_t half = width / 2;
    int32_t ox = (int32_t)(((int64_t)nx * half) / 256);
    int32_t oy = (int32_t)(((int64_t)ny * half) / 256);
    mm_pt q[4];
    q[0].x = p0.x + ox;
    q[0].y = p0.y + oy;
    q[1].x = p1.x + ox;
    q[1].y = p1.y + oy;
    q[2].x = p1.x - ox;
    q[2].y = p1.y - oy;
    q[3].x = p0.x - ox;
    q[3].y = p0.y - oy;
    mm_fill_polygon(c, q, 4, v);
}

/* Two pixel wide stroke: one Bresenham walk writing two adjacent pixels per
 * step.  The generic path built a quad per segment and ran the scanline fill
 * on it (isqrt + qsort + per-row divisions); 2 px is the dominant width at
 * z <= 12 and it was the frame-time killer on the device.  Joins stay the
 * same round disks, so the line looks identical at this size. */
static void draw_line2(mm_canvas *c, mm_pt a, mm_pt b, uint8_t v)
{
    int x0 = mm_px(a.x), y0 = mm_px(a.y), x1 = mm_px(b.x), y1 = mm_px(b.y);
    int cx0 = clip_x0(c), cx1 = clip_x1(c), cy0 = clip_y0(c), cy1 = clip_y1(c);
    const int adx = abs(x1 - x0), ady = abs(y1 - y0);
    const int ox = adx >= ady ? 0 : 1;   /* grow across the minor axis */
    const int oy = adx >= ady ? 1 : 0;
    /* rejection on the full band: the offset pixel may be the only one inside
     * the clip (a vertical road just outside a strip edge) */
    const int minx = (x0 < x1 ? x0 : x1), maxx = (x0 > x1 ? x0 : x1) + ox;
    const int miny = (y0 < y1 ? y0 : y1), maxy = (y0 > y1 ? y0 : y1) + oy;
    if (maxx < cx0 || minx > cx1 || maxy < cy0 || miny > cy1)
        return;
    int dx = adx, sx = x0 < x1 ? 1 : -1;
    int dy = -ady, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(c, x0, y0, v);
        put_px(c, x0 + ox, y0 + oy, v);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void mm_stroke_polyline(mm_canvas *c, const mm_pt *pts, int n, int32_t width,
                        uint8_t v, uint32_t rec_flags)
{
    if (n < 2)
        return;
    /* The band and the join disks extend width/2 beyond the centreline: the
     * bbox cull must not drop a stroke that only crosses the clip edge with
     * its width (that was a 1 px hairline missing at strip boundaries). */
    mm_rect hit = c->clip;
    const int32_t grow = width / 2 + MM_FP_ONE;
    hit.x0 -= grow;
    hit.y0 -= grow;
    hit.x1 += grow;
    hit.y1 += grow;
    if (!mm_bbox_hits(pts, n, hit))
        return;

    if (width <= MM_FP_ONE) {
        for (int i = 0; i + 1 < n; i++)
            mm_draw_line(c, pts[i], pts[i + 1], v);
        return;
    }

    if (width <= 2 * MM_FP_ONE) {
        for (int i = 0; i + 1 < n; i++)
            draw_line2(c, pts[i], pts[i + 1], v);
    } else {
        for (int i = 0; i + 1 < n; i++)
            fill_band(c, pts[i], pts[i + 1], width, v);
    }

    /* round joins at interior vertices, plus the ends that fall on real
     * vertices (a way split at a chunk border keeps its join) */
    int32_t r = width / 2;
    int join_start = !(rec_flags & MM_REC_BUTT_START);
    int join_end = !(rec_flags & MM_REC_BUTT_END);
    for (int i = 0; i < n; i++) {
        if (i > 0 && i + 1 < n) {
            mm_draw_disk(c, pts[i], r, v);
            continue;
        }
        if (i == 0 && join_start)
            mm_draw_disk(c, pts[i], r, v);
        if (i + 1 == n && join_end)
            mm_draw_disk(c, pts[i], r, v);
    }
}
