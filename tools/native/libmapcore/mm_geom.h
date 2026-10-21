/* Polyline geometry helpers: Douglas-Peucker simplification and clipping.
 *
 * Iterative (no recursion): the device runs this with an 8 KB loop task stack,
 * and the host fuzzers feed it adversarial shapes.  Integer/double free of the
 * device path where it matters: clipping runs on fixed point view coordinates,
 * simplification runs at generation time only (host).
 */
#ifndef MM_GEOM_H
#define MM_GEOM_H

#include "mm_core.h"

/* --- generation time (host) ---------------------------------------------- */

/* Douglas-Peucker on integer points, squared tolerance. out must hold n
 * points; returns the kept count (endpoints always kept). flags==NULL keeps
 * only shape; when flags is given, each input point's flag byte is carried to
 * its output point (used for MM_REC_* end markers). Iterative stack lives on
 * the heap. Returns -1 on allocation failure. */
int mm_dp_simplify(const mm_pt *in, int n, int32_t tol2, mm_pt *out,
                   const uint8_t *flags, uint8_t *out_flags);

/* --- runtime (host + device) --------------------------------------------- */

/* Liang-Barsky segment clip against a closed rect (fixed point, 24.8 view
 * space). Returns 1 and rewrites the endpoints when any part is inside. */
int mm_clip_segment(mm_pt *a, mm_pt *b, mm_rect r);

/* Rectangle intersection. */
mm_rect mm_rect_intersect(mm_rect a, mm_rect b);

/* 1 when the polyline bbox can touch the rect (used to skip work early). */
int mm_bbox_hits(const mm_pt *pts, int n, mm_rect r);

#endif /* MM_GEOM_H */
