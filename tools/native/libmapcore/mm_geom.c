#include "mm_geom.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- simplify -- */

typedef struct {
    int32_t a, b;
} dp_range;

int mm_dp_simplify(const mm_pt *in, int n, int32_t tol2, mm_pt *out,
                   const uint8_t *flags, uint8_t *out_flags)
{
    if (n <= 0)
        return 0;
    if (n <= 2 || tol2 <= 0) {
        memcpy(out, in, sizeof(mm_pt) * (size_t)n);
        if (flags && out_flags)
            memcpy(out_flags, flags, (size_t)n);
        return n;
    }

    unsigned char *keep = (unsigned char *)calloc((size_t)n, 1);
    dp_range *stack = (dp_range *)malloc(sizeof(dp_range) * (size_t)n);
    if (!keep || !stack) {
        free(keep);
        free(stack);
        return -1;
    }
    int32_t top = 0;
    keep[0] = keep[n - 1] = 1;
    stack[top].a = 0;
    stack[top].b = n - 1;
    top++;
    const int64_t tol2_64 = tol2;

    while (top > 0) {
        dp_range r = stack[--top];
        if (r.b <= r.a + 1)
            continue;
        int64_t ax = in[r.a].x, ay = in[r.a].y;
        int64_t dx = (int64_t)in[r.b].x - ax, dy = (int64_t)in[r.b].y - ay;
        int64_t len2 = dx * dx + dy * dy;
        int64_t best = -1;
        int32_t besti = -1;
        for (int32_t i = r.a + 1; i < r.b; i++) {
            int64_t ex = (int64_t)in[i].x - ax, ey = (int64_t)in[i].y - ay;
            int64_t d;
            if (len2 <= 0) {
                d = ex * ex + ey * ey;
            } else {
                /* distance squared from the chord, scaled by len2:
                 * d * len2 = (ex*dy - ey*dx)^2 - saturate before squaring so
                 * adversarial inputs cannot overflow (UBSan fuzz target) */
                int64_t cross = ex * dy - ey * dx;
                int64_t abs_cross = cross < 0 ? -cross : cross;
                if (abs_cross > ((int64_t)1 << 31))
                    d = INT64_MAX;
                else
                    d = (abs_cross * abs_cross) / len2;
            }
            if (d > best) {
                best = d;
                besti = i;
            }
        }
        if (best > tol2_64 && besti > 0) {
            keep[besti] = 1;
            /* LIFO: the smaller half is processed first, so the explicit
             * stack stays shallow even on adversarial shapes */
            int32_t lenl = besti - r.a, lenr = r.b - besti;
            dp_range first = {r.a, besti}, second = {besti, r.b};
            if (lenl > lenr) {
                stack[top++] = first;
                stack[top++] = second;
            } else {
                stack[top++] = second;
                stack[top++] = first;
            }
        }
    }

    int m = 0;
    for (int i = 0; i < n; i++) {
        if (keep[i]) {
            out[m] = in[i];
            if (flags && out_flags)
                out_flags[m] = flags[i];
            m++;
        }
    }
    free(keep);
    free(stack);
    return m;
}

/* --------------------------------------------------------------- clip ---- */

static int64_t clip_div(int64_t num, int64_t den)
{
    /* truncation is fine: a clip point a fraction of a pixel outside the rect
     * only means the rasteriser guards the canvas edge anyway */
    if (den == 0)
        return 0;
    return num / den;
}

int mm_clip_segment(mm_pt *a, mm_pt *b, mm_rect r)
{
    int64_t x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y;
    int64_t dx = x1 - x0, dy = y1 - y0;

    /* Liang-Barsky with exact rational parameters: entry/exit stay as
     * numerator/denominator pairs so the clipped endpoints carry no fixed
     * point rounding (a seam at chunk borders would show up as a hairline).
     * Coordinate contract: |v| <= 2^26 in 24.8 (the renderer rejects chunks
     * whose bbox is far outside the view before calling this). */
    int64_t t0n = 0, t0d = 1; /* entry parameter */
    int64_t t1n = 1, t1d = 1; /* exit parameter */
    const int64_t p[4] = {-dx, dx, -dy, dy};
    const int64_t q[4] = {x0 - r.x0, r.x1 - x0, y0 - r.y0, r.y1 - y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0)
                return 0;
            continue;
        }
        int64_t num = q[i], den = p[i];
        if (den < 0) {
            num = -num;
            den = -den; /* t = num/den, den > 0: entry candidate */
            if (num * t0d > t0n * den) {
                t0n = num;
                t0d = den;
            }
        } else {
            if (num * t1d < t1n * den) { /* exit candidate */
                t1n = num;
                t1d = den;
            }
        }
    }
    if (t0n < 0) { /* segment starts inside */
        t0n = 0;
        t0d = 1;
    }
    if (t1n > t1d) { /* segment ends inside */
        t1n = 1;
        t1d = 1;
    }
    if (t0n * t1d > t1n * t0d)
        return 0;

    if (t0n != 0 || t0d != 1) {
        a->x = (int32_t)(x0 + clip_div(dx * t0n, t0d));
        a->y = (int32_t)(y0 + clip_div(dy * t0n, t0d));
    }
    if (t1n != 1 || t1d != 1) {
        b->x = (int32_t)(x0 + clip_div(dx * t1n, t1d));
        b->y = (int32_t)(y0 + clip_div(dy * t1n, t1d));
    }
    return 1;
}

mm_rect mm_rect_intersect(mm_rect a, mm_rect b)
{
    mm_rect r;
    r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    return r;
}

int mm_bbox_hits(const mm_pt *pts, int n, mm_rect r)
{
    if (n <= 0)
        return 0;
    int32_t minx = pts[0].x, maxx = pts[0].x;
    int32_t miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].x < minx) minx = pts[i].x;
        if (pts[i].x > maxx) maxx = pts[i].x;
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    return !(maxx < r.x0 || minx > r.x1 || maxy < r.y0 || miny > r.y1);
}
