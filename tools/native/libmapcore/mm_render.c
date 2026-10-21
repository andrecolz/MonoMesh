#include "mm_render.h"

#include "mm_geom.h"
#include "mm_raster.h"
#include "mm_style.h"
#include "mm_text.h"
#include "mm_varint.h"

#include <math.h>
#include <stdlib.h>

/* Largest record the renderer materialises: the writer splits fills above
 * this and strokes stay well below (see vmap_tool.c). */
#define MM_RENDER_MAX_PTS 1200
#include <stdlib.h>
#include <string.h>

/* Chunk-local units: 1/16 px at the band's grid zoom. */
#define CHUNK_UNIT_BITS 4
#define CHUNK_CELL_UNITS (256 << CHUNK_UNIT_BITS)

typedef struct {
    int64_t origin16_x, origin16_y; /* view top left, 1/16 px at grid_z */
    int32_t scale_raw;              /* 24.8 px per 1/16 px unit */
    int grid_z;
} band_ctx;

typedef struct {
    const mm_vmap *map;
    const mm_chunks *src;
    const mm_view *view;
    mm_canvas *canvas;
    mm_theme theme;
    band_ctx bc;
    int band;
    int32_t z8; /* zoom in 8.8 fixed point for the style tables */
} render_ctx;

static void band_setup(const mm_vmap *map, const mm_view *v, int band, band_ctx *bc)
{
    const mm_vmap_band *b = &map->bands[band];
    bc->grid_z = b->grid_z;
    double s = pow(2.0, b->grid_z - v->z); /* grid px per view px */
    /* view top-left in grid units: both the centre and the half viewport must
     * be scaled to grid px (a stale version scaled only the centre, which
     * shoved every view by (width/2, height/2) whenever s != 1) */
    bc->origin16_x = (int64_t)llround((v->cx - v->width / 2.0) * s * 16.0);
    bc->origin16_y = (int64_t)llround((v->cy - v->height / 2.0) * s * 16.0);
    /* 1 unit = 1/16 px at grid_z = 2^(z-grid_z)/16 view px = 16*2^(z-grid_z)
     * raw 24.8 px, so the per-point transform is a plain multiplication */
    bc->scale_raw = (int32_t)llround(16.0 * pow(2.0, v->z - b->grid_z));
    if (bc->scale_raw < 1)
        bc->scale_raw = 1;
}

static inline int32_t to_view_x(const band_ctx *bc, int32_t cell_x, int32_t local16)
{
    int64_t wx = (int64_t)cell_x * CHUNK_CELL_UNITS + local16 - bc->origin16_x;
    return (int32_t)(wx * bc->scale_raw);
}

static inline int32_t to_view_y(const band_ctx *bc, int32_t cell_y, int32_t local16)
{
    int64_t wy = (int64_t)cell_y * CHUNK_CELL_UNITS + local16 - bc->origin16_y;
    return (int32_t)(wy * bc->scale_raw);
}

int mm_render_band_for(const mm_vmap *map, double z)
{
    int best = -1;
    double best_d = 1e9;
    for (int i = 0; i < map->n_bands; i++) {
        const mm_vmap_band *b = &map->bands[i];
        if (z >= b->zmin && z <= b->zmax + 0.999)
            return i;
        double d = z < b->zmin ? (double)b->zmin - z : z - b->zmax;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    /* within one octave of a band the nearest band still scales sanely; far
     * outside, the fixed point scale would clamp and distort the geometry */
    return best_d <= 1.0 ? best : -1;
}

/* ------------------------------------------------------------- drawing --- */

enum { PASS_FILLS = 0, PASS_HOLES = 1, PASS_LINES = 2 };

static int record_pass(const mm_chunk_reader *r)
{
    if (!(r->flags & MM_REC_FILL))
        return PASS_LINES;
    return r->cls == MM_CLS_WATER_RING_INNER ? PASS_HOLES : PASS_FILLS;
}

/* Draws one decoded record at cell (cx, cy). */
static void draw_record(render_ctx *rc, const mm_chunk_reader *rec, int32_t cell_x,
                        int32_t cell_y, int pass)
{
    /* Point buffer from the canvas scratch arena (the loop task stack is
     * 16 KB: a 38k point coastline ring must never live on the stack), with
     * the raster primitives borrowing the second half for their edge lists. */
    mm_canvas *c = rc->canvas;
    uint8_t *scratch = (uint8_t *)c->scratch;
    size_t scratch_len = c->scratch_len;
    if (!scratch || scratch_len < 2048)
        return;
    /* points get exactly what a record can hold, the rest of the arena is for
     * the rasteriser edge lists (36 B/point: 45 KB hold a 1200 point ring) */
    size_t max_pts = (scratch_len - 48 * 1024) / sizeof(mm_pt);
    if (max_pts > MM_RENDER_MAX_PTS + 2)
        max_pts = MM_RENDER_MAX_PTS + 2;
    if (max_pts < 64)
        return;
    mm_pt *pts = (mm_pt *)scratch;
    size_t half = ((max_pts + 2) * sizeof(mm_pt) + 7) & ~(size_t)7;

    int n = 0;
    pts[n].x = to_view_x(&rc->bc, cell_x, rec->x);
    pts[n].y = to_view_y(&rc->bc, cell_y, rec->y);
    n++;

    mm_chunk_reader r = *rec; /* the caller has the first point decoded */
    while (mm_chunk_next_point(&r) > 0) {
        if (n >= (int)max_pts)
            break; /* writer caps records at MM_RENDER_MAX_PTS */
        pts[n].x = to_view_x(&rc->bc, cell_x, r.x);
        pts[n].y = to_view_y(&rc->bc, cell_y, r.y);
        n++;
    }

    int cls = rec->cls;
    uint32_t flags = rec->flags;
    int is_fill = (flags & MM_REC_FILL) != 0;

    if (is_fill && pass == PASS_LINES)
        return;
    if (!is_fill && pass != PASS_LINES)
        return;
    if (is_fill && pass == PASS_FILLS && cls == MM_CLS_WATER_RING_INNER)
        return;
    if (is_fill && pass == PASS_HOLES && cls != MM_CLS_WATER_RING_INNER)
        return;
    /* style zoom gate for water lines/areas (roads gate through their width):
     * streams at z<13 or rivers at z<10 are stored in the band but hidden */
    if (cls > MM_CLS_MOTORWAY_LINK && cls != MM_CLS_RAILWAY &&
        !mm_style_class_visible(cls, rc->z8))
        return;

    void *saved_scratch = c->scratch;
    size_t saved_len = c->scratch_len;
    c->scratch = scratch + half;
    c->scratch_len = scratch_len - half;

    if (is_fill) {
        if (n < 3)
            goto out;
        /* tiny water bodies vanish below their visibility size */
        int32_t min_size = mm_style_water_min_size(rc->z8);
        if (min_size > 0 && cls != MM_CLS_WATER_RING_INNER) {
            int32_t minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
            for (int i = 1; i < n; i++) {
                if (pts[i].x < minx) minx = pts[i].x;
                if (pts[i].x > maxx) maxx = pts[i].x;
                if (pts[i].y < miny) miny = pts[i].y;
                if (pts[i].y > maxy) maxy = pts[i].y;
            }
            if (maxx - minx < min_size && maxy - miny < min_size)
                goto out;
        }
        uint8_t color = (cls == MM_CLS_WATER_RING_INNER) ? rc->theme.bg : rc->theme.fg;
        mm_fill_polygon(c, pts, n, color);
        goto out;
    }

    int32_t width = 0;
    int casing = 0;
    if (cls <= MM_CLS_MOTORWAY_LINK) {
        width = mm_style_road_width(cls, rc->z8, &casing);
        if (width <= 0)
            goto out;
        if (casing && width >= 3 * MM_FP_ONE) {
            mm_stroke_polyline(c, pts, n, width, rc->theme.fg, flags);
            mm_stroke_polyline(c, pts, n, width - 2 * MM_FP_ONE, rc->theme.bg, flags);
            goto out;
        }
    } else if (cls == MM_CLS_RAILWAY) {
        width = MM_FP_ONE;
    } else {
        width = mm_style_water_line_width(rc->z8);
    }
    mm_stroke_polyline(c, pts, n, width, rc->theme.fg, flags);

out:
    c->scratch = saved_scratch;
    c->scratch_len = saved_len;
}

static int record_pass_dir(const uint8_t *m)
{
    if (!(m[1] & MM_REC_FILL))
        return PASS_LINES;
    return m[0] == MM_CLS_WATER_RING_INNER ? PASS_HOLES : PASS_FILLS;
}

/* Same point-bbox semantics as mm_bbox_hits() on the transformed polyline:
 * the transform is monotonic, so the bounds of the transformed corners are the
 * bounds of the transformed points.  The encoded bounds are floored/ceiled
 * outward (2 grid px units), so the test only ever over-includes. */
static int dir_bbox_hits(const render_ctx *rc, const uint8_t *m,
                         int32_t cell_x, int32_t cell_y)
{
    int32_t x0 = to_view_x(&rc->bc, cell_x, (int32_t)m[2] * MM_DIR_BBOX_UNIT);
    int32_t x1 = to_view_x(&rc->bc, cell_x, (int32_t)m[4] * MM_DIR_BBOX_UNIT);
    int32_t y0 = to_view_y(&rc->bc, cell_y, (int32_t)m[3] * MM_DIR_BBOX_UNIT);
    int32_t y1 = to_view_y(&rc->bc, cell_y, (int32_t)m[5] * MM_DIR_BBOX_UNIT);
    if (x1 < x0) {
        int32_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        int32_t t = y0;
        y0 = y1;
        y1 = t;
    }
    /* Same margin the stroke cull uses (max stroke half width + join): the
     * pre-cull must never be tighter than the primitive's own bbox test. */
    const mm_rect *c = &rc->canvas->clip;
    const int32_t grow = 8 * MM_FP_ONE;
    return !(x1 < c->x0 - grow || x0 > c->x1 + grow || y1 < c->y0 - grow ||
             y0 > c->y1 + grow);
}

/* Iterates the visible cells of one pass. */
static int render_pass(render_ctx *rc, int pass)
{
    const mm_vmap *map = rc->map;
    const mm_vmap_band *b = &map->bands[rc->band];
    const band_ctx *bc = &rc->bc;

    /* Visible region: viewport ∩ canvas clip, plus the usual 32 view px margin
     * (records are clipped to their cell, the margin covers stroke width and
     * joins).  A strip repaint only touches the cells that can really write a
     * pixel - at low zoom a strip crosses a fraction of the viewport's cells.
     * A full-canvas render keeps the old range. */
    int32_t vx0 = mm_px(rc->canvas->clip.x0);
    int32_t vx1 = mm_px(rc->canvas->clip.x1);
    int32_t vy0 = mm_px(rc->canvas->clip.y0);
    int32_t vy1 = mm_px(rc->canvas->clip.y1);
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > rc->view->width - 1) vx1 = rc->view->width - 1;
    if (vy1 > rc->view->height - 1) vy1 = rc->view->height - 1;
    if (vx1 < vx0 || vy1 < vy0)
        return 0; /* clip outside the viewport: nothing to draw */

    int64_t margin = (int64_t)(32 * MM_FP_ONE) / bc->scale_raw;
    int64_t x0 = bc->origin16_x + ((int64_t)vx0 * MM_FP_ONE) / bc->scale_raw - margin;
    int64_t y0 = bc->origin16_y + ((int64_t)vy0 * MM_FP_ONE) / bc->scale_raw - margin;
    int64_t x1 = bc->origin16_x + ((int64_t)vx1 * MM_FP_ONE) / bc->scale_raw + margin;
    int64_t y1 = bc->origin16_y + ((int64_t)vy1 * MM_FP_ONE) / bc->scale_raw + margin;

    int32_t cx0 = (int32_t)(x0 >> (8 + CHUNK_UNIT_BITS));
    int32_t cy0 = (int32_t)(y0 >> (8 + CHUNK_UNIT_BITS));
    int32_t cx1 = (int32_t)(x1 >> (8 + CHUNK_UNIT_BITS));
    int32_t cy1 = (int32_t)(y1 >> (8 + CHUNK_UNIT_BITS));
    /* 64 bit bounds: a corrupt grid header must not wrap the u32 sums */
    if (cx0 < (int32_t)b->x0) cx0 = (int32_t)b->x0;
    if (cy0 < (int32_t)b->y0) cy0 = (int32_t)b->y0;
    if ((uint64_t)cx1 > (uint64_t)b->x0 + b->nx - 1)
        cx1 = (int32_t)((uint64_t)b->x0 + b->nx - 1);
    if ((uint64_t)cy1 > (uint64_t)b->y0 + b->ny - 1)
        cy1 = (int32_t)((uint64_t)b->y0 + b->ny - 1);

    for (int32_t cy = cy0; cy <= cy1; cy++) {
        for (int32_t cx = cx0; cx <= cx1; cx++) {
            const uint8_t *data = NULL;
            size_t len = 0;
            uint16_t cflags = 0;
            int rc2 = rc->src->get_chunk(rc->src->user, rc->band, (uint32_t)cx,
                                         (uint32_t)cy, &data, &len, &cflags);
            if (rc2 < 0)
                return -1;
            if (rc2 > 0 || !data || len == 0)
                continue;
            // Fast path: a cached record directory lets the pass cull whole
            // records (pass mismatch, bbox outside the clip) before decoding
            // any point.  Pixel identical to the legacy walk below.
            const mm_chunk_dir *dir = NULL;
            if (rc->src->get_dir) {
                int rd = rc->src->get_dir(rc->src->user, rc->band, (uint32_t)cx,
                                          (uint32_t)cy, &dir);
                if (rd < 0)
                    return -1;
                if (rd > 0)
                    dir = NULL;
            }
            if (dir) {
                for (uint32_t i = 0; i < dir->n; i++) {
                    const uint8_t *m = &dir->meta[MM_DIR_META_STRIDE * i];
                    if (record_pass_dir(m) != pass)
                        continue;
                    if (!dir_bbox_hits(rc, m, cx, cy))
                        continue;
                    mm_chunk_reader r;
                    mm_chunk_reader_init(&r, data, len);
                    r.pos = dir->pos[i];
                    if (mm_chunk_next_record(&r) != 0)
                        continue; /* corrupt record: the directory is stale */
                    draw_record(rc, &r, cx, cy, pass);
                }
                continue;
            }
            mm_chunk_reader r;
            mm_chunk_reader_init(&r, data, len);
            for (;;) {
                int rr = mm_chunk_next_record(&r);
                if (rr < 0)
                    break; /* corrupt record: skip the rest of the chunk */
                if (rr > 0)
                    break;
                if (record_pass(&r) == pass)
                    draw_record(rc, &r, cx, cy, pass);
                /* skip the points we did not consume */
                while (mm_chunk_next_point(&r) > 0) {
                }
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------- labels --- */

typedef struct {
    int32_t score;
    uint32_t poi;
    char name[64];
} label_cand;

typedef struct {
    label_cand *items;
    size_t n, cap;
    const mm_vmap *map;
    int32_t z8;
} cand_list;

static void collect_cb(void *user, uint32_t poi_index, int32_t score)
{
    cand_list *cl = (cand_list *)user;
    mm_vmap_poi p;
    char name[64];
    if (mm_vmap_poi_get(cl->map, poi_index, &p) != 0)
        return;
    if (!mm_style_label_visible(p.kind, cl->z8))
        return;
    if (mm_vmap_poi_name(cl->map, &p, name, sizeof(name)) <= 0)
        return;
    if (cl->n == cl->cap) {
        size_t cap = cl->cap ? cl->cap * 2 : 256;
        label_cand *grown = (label_cand *)realloc(cl->items, sizeof(label_cand) * cap);
        if (!grown)
            return;
        cl->items = grown;
        cl->cap = cap;
    }
    label_cand *c = &cl->items[cl->n++];
    c->score = score;
    c->poi = poi_index;
    memcpy(c->name, name, sizeof(name));
}

static int cmp_cand(const void *a, const void *b)
{
    const label_cand *x = a, *y = b;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    return strcmp(x->name, y->name); /* deterministic tie break (plan D5) */
}

typedef struct {
    int32_t x0, y0, x1, y1;
} lbox;

static int boxes_clash(const lbox *a, const lbox *b)
{
    const int32_t pad = MM_FP_ONE;
    return !(a->x1 + pad <= b->x0 || b->x1 + pad <= a->x0 ||
             a->y1 + pad <= b->y0 || b->y1 + pad <= a->y0);
}

void mm_label_cache_reset(mm_label_cache *cache)
{
    if (cache)
        cache->valid = 0;
}

static void draw_label(render_ctx *rc, int32_t cx, int32_t cy, uint8_t dot_r,
                       const char *name, int variant)
{
    int32_t tx = cx + ((int32_t)dot_r + 3) * MM_FP_ONE;
    int32_t ty = cy - 6 * MM_FP_ONE;
    mm_draw_disk(rc->canvas, (mm_pt){cx, cy}, (int32_t)dot_r * MM_FP_ONE, rc->theme.fg);
    mm_text_draw(rc->canvas, tx, ty, name, variant, rc->theme.fg, rc->theme.bg);
}

/* Full layout rebuild for the current viewport: same placement rules as
 * before the cache existed (score order, name tie-break, padded boxes), so a
 * fresh cache always matches an uncached render pixel for pixel. */
static void rebuild_labels(render_ctx *rc, mm_label_cache *cache)
{
    const mm_vmap *map = rc->map;
    /* POI grid cells overlapping the view (labels may bleed in from outside);
     * both the centre and the half viewport scale to grid units */
    double s = pow(2.0, map->poigrid_z - rc->view->z);
    double left = (rc->view->cx - rc->view->width / 2.0 - 64) * s;
    double right = (rc->view->cx + rc->view->width / 2.0 + 64) * s;
    double top = (rc->view->cy - rc->view->height / 2.0 - 64) * s;
    double bottom = (rc->view->cy + rc->view->height / 2.0 + 64) * s;
    uint32_t tx0 = (uint32_t)(int64_t)floor(left / 256.0);
    uint32_t ty0 = (uint32_t)(int64_t)floor(top / 256.0);
    uint32_t tx1 = (uint32_t)(int64_t)floor(right / 256.0);
    uint32_t ty1 = (uint32_t)(int64_t)floor(bottom / 256.0);

    int n_placed = 0;
    if (cache) {
        cache->map = map;
        cache->z = rc->view->z;
        cache->cx = rc->view->cx;
        cache->cy = rc->view->cy;
        cache->width = rc->view->width;
        cache->height = rc->view->height;
        cache->fg = rc->theme.fg;
        cache->bg = rc->theme.bg;
        cache->n = 0;
        cache->valid = 1;
        cache->rebuilds++;
    }

    cand_list cl;
    memset(&cl, 0, sizeof(cl));
    cl.map = map;
    cl.z8 = rc->z8;
    if (mm_vmap_poi_foreach(map, tx0, ty0, tx1, ty1, collect_cb, &cl) != 0) {
        free(cl.items);
        return;
    }
    if (cl.n == 0) {
        free(cl.items);
        return;
    }
    qsort(cl.items, cl.n, sizeof(label_cand), cmp_cand);

    int budget = mm_style_label_budget(rc->z8);
    lbox placed[MM_LABEL_CACHE_MAX];
    if (budget > MM_LABEL_CACHE_MAX)
        budget = MM_LABEL_CACHE_MAX;

    for (size_t i = 0; i < cl.n && n_placed < budget; i++) {
        mm_vmap_poi p;
        if (mm_vmap_poi_get(map, cl.items[i].poi, &p) != 0)
            continue;
        int variant = mm_style_label_font(p.kind, rc->z8);
        if (variant < 0)
            continue;
        /* POI position in view px */
        double s2 = pow(2.0, rc->view->z);
        double px = (p.lon_e7 / 1e7 + 180.0) / 360.0 * s2 * 256.0;
        double py = (1.0 - asinh(tan((p.lat_e7 / 1e7) * M_PI / 180.0)) / M_PI) / 2.0 *
                    s2 * 256.0;
        int32_t cx = (int32_t)llround((px - (rc->view->cx - rc->view->width / 2.0)) * MM_FP_ONE);
        int32_t cy = (int32_t)llround((py - (rc->view->cy - rc->view->height / 2.0)) * MM_FP_ONE);

        int dot_r = mm_style_label_dot_radius(p.kind);
        int32_t tx = cx + (dot_r + 3) * MM_FP_ONE;
        int32_t ty = cy - 6 * MM_FP_ONE;
        int32_t tw, ytop, ybot;
        mm_text_metrics(cl.items[i].name, variant, &tw, &ytop, &ybot);
        lbox box = {tx, ty + ytop - 2 * MM_FP_ONE, tx + tw, ty + ybot + 2 * MM_FP_ONE};
        if (box.x1 < 0 || box.x0 > rc->view->width * MM_FP_ONE ||
            box.y1 < 0 || box.y0 > rc->view->height * MM_FP_ONE)
            continue;
        int clash = 0;
        for (int k = 0; k < n_placed; k++) {
            if (boxes_clash(&box, &placed[k])) {
                clash = 1;
                break;
            }
        }
        if (clash)
            continue;
        placed[n_placed] = box;
        if (cache) {
            mm_cached_label *it = &cache->items[n_placed];
            it->x = cx;
            it->y = cy;
            it->variant = (uint8_t)variant;
            it->dot_r = (uint8_t)dot_r;
            memcpy(it->name, cl.items[i].name, sizeof(it->name));
            /* full box, dot included: the cached draw culls against it */
            const int32_t r = (int32_t)dot_r * MM_FP_ONE;
            it->bx0 = box.x0 < cx - r ? box.x0 : cx - r;
            it->by0 = box.y0 < cy - r ? box.y0 : cy - r;
            it->bx1 = box.x1 > cx + r ? box.x1 : cx + r;
            it->by1 = box.y1 > cy + r ? box.y1 : cy + r;
        }
        draw_label(rc, cx, cy, (uint8_t)dot_r, cl.items[i].name, variant);
        n_placed++;
    }
    if (cache)
        cache->n = n_placed;
    free(cl.items);
}

/* Strip repaint inside the slack: redraw the cached layout shifted by the pan
 * (the view centre moved, the world anchor did not). */
static void draw_cached_labels(render_ctx *rc, const mm_label_cache *cache)
{
    int32_t dx = (int32_t)llround((cache->cx - rc->view->cx) * MM_FP_ONE);
    int32_t dy = (int32_t)llround((cache->cy - rc->view->cy) * MM_FP_ONE);
    const mm_rect *clip = &rc->canvas->clip;
    for (int i = 0; i < cache->n; i++) {
        const mm_cached_label *it = &cache->items[i];
        /* labels are clipped when drawn anyway: skipping the ones whose box
         * misses the clip is pixel exact and keeps a strip repaint cheap */
        if (it->bx1 + dx < clip->x0 || it->bx0 + dx > clip->x1 ||
            it->by1 + dy < clip->y0 || it->by0 + dy > clip->y1)
            continue;
        draw_label(rc, it->x + dx, it->y + dy, it->dot_r, it->name, it->variant);
    }
}

static void render_labels(render_ctx *rc, mm_label_cache *cache, int allow_rebuild)
{
    const mm_vmap *map = rc->map;
    if (!map->has_poigrid)
        return;
    if (cache && cache->valid && cache->map == map && cache->z == rc->view->z &&
        cache->width == rc->view->width && cache->height == rc->view->height &&
        cache->fg == rc->theme.fg && cache->bg == rc->theme.bg) {
        const int in_slack = fabs(cache->cx - rc->view->cx) <= MM_LABEL_CACHE_SLACK_PX &&
                             fabs(cache->cy - rc->view->cy) <= MM_LABEL_CACHE_SLACK_PX;
        if (in_slack || !allow_rebuild) {
            draw_cached_labels(rc, cache);
            return;
        }
    }
    rebuild_labels(rc, cache);
}

/* --------------------------------------------------------------- entry --- */

int mm_render_view_cached_ex(const mm_vmap *map, const mm_chunks *src,
                             const mm_view *view, mm_canvas *canvas,
                             const mm_theme *theme, mm_label_cache *cache,
                             int allow_rebuild)
{
    if (!map || !src || !view || !canvas)
        return -1;
    if (map->n_bands == 0)
        return -1;

    render_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.map = map;
    rc.src = src;
    rc.view = view;
    rc.canvas = canvas;
    rc.theme = *theme;
    rc.band = mm_render_band_for(map, view->z);
    if (rc.band < 0)
        return -1;
    rc.z8 = (int32_t)llround(view->z * 256.0);
    band_setup(map, view, rc.band, &rc.bc);
    if (mm_text_init() != 0)
        return -1;

    /* passes: fills, island holes, then everything stroked */
    if (render_pass(&rc, PASS_FILLS) != 0)
        return -1;
    if (render_pass(&rc, PASS_HOLES) != 0)
        return -1;
    if (render_pass(&rc, PASS_LINES) != 0)
        return -1;
    render_labels(&rc, cache, allow_rebuild);
    return 0;
}

int mm_render_view_cached(const mm_vmap *map, const mm_chunks *src, const mm_view *view,
                          mm_canvas *canvas, const mm_theme *theme,
                          mm_label_cache *cache)
{
    return mm_render_view_cached_ex(map, src, view, canvas, theme, cache, 1);
}

int mm_render_view(const mm_vmap *map, const mm_chunks *src, const mm_view *view,
                   mm_canvas *canvas, const mm_theme *theme)
{
    return mm_render_view_cached(map, src, view, canvas, theme, NULL);
}
