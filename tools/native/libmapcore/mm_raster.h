/* Fixed point rasteriser (plan D4): 1px lines, wide strokes with round joins,
 * polygon fills and text with runtime halo.  Integer pixel-centre convention
 * (same as the Pillow raster pipeline the golden tests compare against).
 *
 * Single rounding rule: every primitive converts 24.8 view coordinates to
 * integer pixels through mm_px() and nothing else, so casings, fills and halos
 * can never disagree by a pixel.
 *
 * No anti-aliasing on lines: on the 1-bit panel grey does not exist and
 * dithering at pan time produces crawling edges.  Coverage blending is only
 * used for glyph masks (the atlas is pre-rendered).
 */
#ifndef MM_RASTER_H
#define MM_RASTER_H

#include "mm_core.h"

/* THE rounding function: 24.8 -> integer pixel (round half up). */
static inline int mm_px(int32_t v24)
{
    return (int)((v24 + 128) >> 8);
}

/* Single pixel write, clipped to the canvas clip rect. */
void mm_canvas_put(mm_canvas *c, int x, int y, uint8_t v);

void mm_canvas_clear(mm_canvas *c, uint8_t v);
void mm_canvas_fill_rect(mm_canvas *c, mm_rect r, uint8_t v);

/* 1px line (Bresenham) between two 24.8 points. */
void mm_draw_line(mm_canvas *c, mm_pt a, mm_pt b, uint8_t v);

/* Filled disk, radius r in 24.8. */
void mm_draw_disk(mm_canvas *c, mm_pt center, int32_t r, uint8_t v);

/* Even-odd polygon fill (closed automatically). */
void mm_fill_polygon(mm_canvas *c, const mm_pt *pts, int n, uint8_t v);

/* Stroke a polyline: 1px uses Bresenham, wider strokes fill one quad per
 * segment plus round join disks at the interior vertices.  Ends are butt
 * caps unless the corresponding MM_REC_BUTT_* flag is CLEAR: a clear flag
 * means the end falls on a real vertex (a way split at a chunk border) and
 * the join disk must be drawn there.  width is 24.8 px. */
void mm_stroke_polyline(mm_canvas *c, const mm_pt *pts, int n, int32_t width,
                        uint8_t v, uint32_t rec_flags);

/* Text: see mm_text.h for the glyph atlas helpers (halo via mask dilation). */

#endif /* MM_RASTER_H */
