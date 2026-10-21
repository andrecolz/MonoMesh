/* View renderer: turns a v2 container + a view into canvas pixels.
 *
 * Shared between the host test tools and the firmware (plan D4/D5):
 *  - world -> viewport is scale + translate today, rotation ready: the API
 *    already goes through a per-band setup, so adding a rotation term later
 *    touches one function;
 *  - drawing happens in passes: water fills, island holes, then strokes
 *    (water lines, roads minor -> major, rails) - so a road crossing a chunk
 *    border is never buried by the neighbour's lake fill;
 *  - pan quantisation and the strip repaint are the caller's business: this
 *    module just draws into the canvas clip rect it is given;
 *  - no double on the hot path: the per-point transform is fixed point.
 */
#ifndef MM_RENDER_H
#define MM_RENDER_H

#include "mm_core.h"
#include "mm_vmap.h"

typedef struct {
    double z;            /* fractional Web Mercator zoom */
    double cx, cy;       /* view centre in world px at zoom z */
    int32_t width, height;
} mm_view;

/* Chunk source: the caller owns IO and caching.  The returned payload must
 * stay valid until the next call.  Returns 0 with data, 1 for an empty cell,
 * -1 on error. */
typedef struct {
    int (*get_chunk)(void *user, int band, uint32_t cell_x, uint32_t cell_y,
                     const uint8_t **data, size_t *len, uint16_t *flags);
    /* Optional record directory for the same cell (cached by the caller).
     * 0 = ok, 1 = empty cell, -1 = error.  NULL disables the fast path. */
    int (*get_dir)(void *user, int band, uint32_t cell_x, uint32_t cell_y,
                   const mm_chunk_dir **dir);
    void *user;
} mm_chunks;

typedef struct {
    uint8_t fg, bg;      /* theme: light = fg 0 / bg 255, dark inverts */
} mm_theme;

/* --------------------------------------------------------- label cache --- */
/* Plan D5: the accepted label set is world anchored and must stay stable
 * between frames and between the 1-bit and the 4-level passes.  The cache
 * stores the labels placed for one viewport (positions in view 24.8); a strip
 * repaint inside the slack reuses them shifted by the pan, without touching
 * the POI sections at all.  A full render (or a pan beyond the slack) rebuilds
 * the layout with exactly the same placement rules, so the cached result is
 * bit identical to an uncached render of the same viewport. */
#define MM_LABEL_CACHE_MAX 24
#define MM_LABEL_CACHE_SLACK_PX 32.0  /* rebuild only past this much pan */

typedef struct {
    int32_t x, y;        /* dot centre, view 24.8 at cache build time */
    int32_t bx0, by0, bx1, by1; /* full box (dot + text), same space: lets a
                                    strip repaint skip labels it cannot touch */
    uint8_t variant;     /* atlas font variant */
    uint8_t dot_r;       /* marker radius in px */
    char name[64];
} mm_cached_label;

typedef struct {
    const mm_vmap *map;  /* container identity: a region switch invalidates */
    int valid;
    double z, cx, cy;    /* view parameters at build time */
    int32_t width, height;
    uint8_t fg, bg;
    int n;
    uint32_t rebuilds;   /* diagnostics: full layout rebuilds so far */
    mm_cached_label items[MM_LABEL_CACHE_MAX];
} mm_label_cache;

void mm_label_cache_reset(mm_label_cache *cache);

/* Draws map + labels for the view.  Returns 0 ok, -1 on source errors. */
int mm_render_view(const mm_vmap *map, const mm_chunks *src, const mm_view *view,
                   mm_canvas *canvas, const mm_theme *theme);

/* Same, but reusing (or filling) a label layout cache.  `cache` may be NULL,
 * in which case this behaves exactly like mm_render_view. */
int mm_render_view_cached(const mm_vmap *map, const mm_chunks *src, const mm_view *view,
                          mm_canvas *canvas, const mm_theme *theme,
                          mm_label_cache *cache);

/* allow_rebuild = 0 keeps a still-valid layout even past the pan slack: strip
 * repaints during a drag reuse the shifted set instead of rebuilding it, so
 * labels do not pop mid-gesture (plan D5 hysteresis).  Full renders pass 1 and
 * refresh the layout.  A cache that does not match the view (zoom, theme,
 * container) is always rebuilt: there is nothing to reuse. */
int mm_render_view_cached_ex(const mm_vmap *map, const mm_chunks *src,
                             const mm_view *view, mm_canvas *canvas,
                             const mm_theme *theme, mm_label_cache *cache,
                             int allow_rebuild);

/* Picks the band index serving zoom z (clamped to the available bands). */
int mm_render_band_for(const mm_vmap *map, double z);

#endif /* MM_RENDER_H */
