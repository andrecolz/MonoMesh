/* Projection, per-tile index, rasteriser, PNG writer and label rendering. */
#include "monomesh.h"
#include "glyph_atlas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TILE_PX 256

/* ------------------------------------------------------------------ grid -- */

static int lon_to_tx(double lon, double n)
{
    return (int)((lon + 180.0) / 360.0 * n);
}

static int lat_to_ty(double lat, double n)
{
    double r = lat * M_PI / 180.0;
    return (int)((1.0 - asinh(tan(r)) / M_PI) / 2.0 * n);
}

mm_grid mm_tile_grid(const mm_data *d, int z)
{
    double n = pow(2.0, z);
    int x0 = lon_to_tx(d->bbox[1], n), x1 = lon_to_tx(d->bbox[3], n);
    int y0 = lat_to_ty(d->bbox[2], n), y1 = lat_to_ty(d->bbox[0], n);
    mm_grid g;
    g.tx0 = x0 < x1 ? x0 : x1;
    g.ty0 = y0 < y1 ? y0 : y1;
    g.nx = abs(x1 - x0) + 1;
    g.ny = abs(y1 - y0) + 1;
    return g;
}

/* ------------------------------------------------------------ projection -- */

double *mm_project_points(const mm_data *d, int ref_z)
{
    double n = pow(2.0, ref_z);
    double *xy = malloc(sizeof(double) * 2 * (size_t)(d->n_points ? d->n_points : 1));
    if (!xy)
        return NULL;
    for (int32_t i = 0; i < d->n_points; i++) {
        double lat = d->lat_e5[i] / MM_E5;
        double lon = d->lon_e5[i] / MM_E5;
        xy[2 * i] = (lon + 180.0) / 360.0 * n;
        xy[2 * i + 1] = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * n;
    }
    return xy;
}

/* ----------------------------------------------------------------- index -- */

typedef struct {
    int32_t way;
    int32_t seq;
    int16_t prio;
} mm_item;

struct mm_index {
    const mm_data *data;
    int z;
    int ref_z;
    mm_grid grid;
    const double *xy;
    double scale;
    mm_item *items;
    int32_t *bucket_start; /* nx*ny + 1 entries */
    int32_t *place_start;
    int32_t *place_items;
    int32_t n_items;
    int32_t n_place_items;
};

static int cmp_item(const void *a, const void *b)
{
    const mm_item *x = a, *y = b;
    if (x->prio != y->prio)
        return x->prio < y->prio ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

/* priority / width / casing for a way at a given zoom */
/* Returns 0 when this way must not be drawn at this zoom at all. */
static int way_style(const mm_way *w, int z, int *prio, int *width, int *casing)
{
    *casing = 0;
    if (w->type == MM_HIGHWAY) {
        if (w->prio >= 17)
            return 0;
        *prio = w->prio;
        *width = mm_road_width(w->prio, z, casing);
        return *width > 0;
    }
    if (w->type == MM_WATER) {
        int kind = w->prio;
        if (!mm_water_kind_drawn(kind, z))
            return 0;
        if (kind == MM_WATER_RING_INNER) {
            *prio = -2;              /* holes are punched after the outer rings */
            *width = w->closed ? 0 : mm_water_line_width(z);
            return 1;
        }
        *prio = -3;
        if (kind == MM_WATER_RING_OUTER) {
            *width = w->closed ? 0 : mm_water_line_width(z);
            return 1;
        }
        if (kind == MM_WATER_AREA)
            *width = w->closed ? 0 : mm_water_line_width(z);
        else
            *width = mm_water_line_width(z);
        return 1;
    }
    *prio = -1;
    *width = 1;
    return 1;
}

/* Projected size of a way in pixels (bounding box, used for water filtering). */
static double way_size_px(const mm_index *ix, const mm_way *w)
{
    double minx = INFINITY, maxx = -INFINITY, miny = INFINITY, maxy = -INFINITY;
    for (int32_t j = 0; j < w->npts; j++) {
        double x = ix->xy[2 * (w->off + j)] * ix->scale;
        double y = ix->xy[2 * (w->off + j) + 1] * ix->scale;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    double wpx = (maxx - minx) * 256.0, hpx = (maxy - miny) * 256.0;
    return wpx > hpx ? wpx : hpx;
}

/* Should this way be part of the tile index? */
static int way_indexable(const mm_index *ix, const mm_way *w)
{
    int prio, width, casing;
    if (!way_style(w, ix->z, &prio, &width, &casing))
        return 0;
    if (w->type == MM_WATER &&
        (w->prio == MM_WATER_AREA || w->prio == MM_WATER_RING_OUTER ||
         w->prio == MM_WATER_RING_INNER)) {
        double min_size = mm_water_min_size(ix->z);
        if (min_size > 0.0 && w->prio != MM_WATER_RING_INNER &&
            way_size_px(ix, w) < min_size)
            return 0;
    }
    return 1;
}


/* Scans the ways once, calling cb with the tile range of every way that is
 * actually rendered (highways with known classes, water, railway). */
static void for_each_rendered_way(const mm_index *ix,
                                  void (*cb)(void *ctx, int32_t way, int tx0, int tx1, int ty0, int ty1),
                                  void *ctx)
{
    const mm_data *d = ix->data;
    for (int32_t i = 0; i < d->n_ways; i++) {
        const mm_way *w = &d->ways[i];
        if (!way_indexable(ix, w))
            continue;
        double minx = INFINITY, maxx = -INFINITY, miny = INFINITY, maxy = -INFINITY;
        for (int32_t j = 0; j < w->npts; j++) {
            double x = ix->xy[2 * (w->off + j)] * ix->scale;
            double y = ix->xy[2 * (w->off + j) + 1] * ix->scale;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
        int tx0 = (int)floor(minx), tx1 = (int)floor(maxx);
        int ty0 = (int)floor(miny), ty1 = (int)floor(maxy);
        if (tx1 < ix->grid.tx0 || tx0 >= ix->grid.tx0 + ix->grid.nx ||
            ty1 < ix->grid.ty0 || ty0 >= ix->grid.ty0 + ix->grid.ny)
            continue;
        if (tx0 < ix->grid.tx0) tx0 = ix->grid.tx0;
        if (ty0 < ix->grid.ty0) ty0 = ix->grid.ty0;
        if (tx1 > ix->grid.tx0 + ix->grid.nx - 1) tx1 = ix->grid.tx0 + ix->grid.nx - 1;
        if (ty1 > ix->grid.ty0 + ix->grid.ny - 1) ty1 = ix->grid.ty0 + ix->grid.ny - 1;
        cb(ctx, i, tx0, tx1, ty0, ty1);
    }
}

static int32_t place_bucket(const mm_index *ix, int32_t pi)
{
    const mm_data *d = ix->data;
    double lat = d->place_lat_e5[pi] / MM_E5, lon = d->place_lon_e5[pi] / MM_E5;
    double n = pow(2.0, ix->z);
    int tx = lon_to_tx(lon, n), ty = lat_to_ty(lat, n);
    if (tx < ix->grid.tx0 || tx >= ix->grid.tx0 + ix->grid.nx ||
        ty < ix->grid.ty0 || ty >= ix->grid.ty0 + ix->grid.ny)
        return -1;
    return (int32_t)((size_t)(ty - ix->grid.ty0) * ix->grid.nx + (tx - ix->grid.tx0));
}

typedef struct {
    int32_t *counts;
    const mm_index *ix;
    mm_item *items;
    int32_t seq;
} fill_ctx;

static void count_cb(void *ctx, int32_t way, int tx0, int tx1, int ty0, int ty1)
{
    fill_ctx *c = ctx;
    for (int tx = tx0; tx <= tx1; tx++)
        for (int ty = ty0; ty <= ty1; ty++)
            c->counts[(size_t)(ty - c->ix->grid.ty0) * c->ix->grid.nx + (tx - c->ix->grid.tx0)]++;
    (void)way;
}

static void fill_cb(void *ctx, int32_t way, int tx0, int tx1, int ty0, int ty1)
{
    fill_ctx *c = ctx;
    const mm_way *w = &c->ix->data->ways[way];
    int prio, width, casing;
    way_style(w, c->ix->z, &prio, &width, &casing);
    (void)width;
    (void)casing;
    for (int tx = tx0; tx <= tx1; tx++) {
        for (int ty = ty0; ty <= ty1; ty++) {
            size_t b = (size_t)(ty - c->ix->grid.ty0) * c->ix->grid.nx + (tx - c->ix->grid.tx0);
            int32_t slot = c->ix->bucket_start[b] + c->counts[b]++;
            c->items[slot].way = way;
            c->items[slot].seq = c->seq++;
            c->items[slot].prio = (int16_t)prio;
        }
    }
}

mm_index *mm_index_build(const mm_data *d, int z, int ref_z, const double *xy)
{
    mm_index *ix = calloc(1, sizeof(*ix));
    if (!ix)
        return NULL;
    ix->data = d;
    ix->z = z;
    ix->ref_z = ref_z;
    ix->xy = xy;
    ix->scale = pow(2.0, z - ref_z);
    ix->grid = mm_tile_grid(d, z);

    size_t nbuckets = (size_t)ix->grid.nx * (size_t)ix->grid.ny;
    int32_t *counts = calloc(nbuckets + 1, sizeof(int32_t));
    int32_t *place_counts = calloc(nbuckets + 1, sizeof(int32_t));
    if (!counts || !place_counts)
        goto fail;

    fill_ctx ctx = {.counts = counts, .ix = ix, .items = NULL, .seq = 0};
    for_each_rendered_way(ix, count_cb, &ctx);
    for (int32_t i = 0; i < d->n_places; i++) {
        int32_t b = place_bucket(ix, i);
        if (b >= 0)
            place_counts[b]++;
    }

    int64_t total = 0, total_places = 0;
    for (size_t i = 0; i < nbuckets; i++) {
        total += counts[i];
        total_places += place_counts[i];
    }
    ix->n_items = (int32_t)total;
    ix->n_place_items = (int32_t)total_places;
    ix->bucket_start = malloc(sizeof(int32_t) * (nbuckets + 1));
    ix->place_start = malloc(sizeof(int32_t) * (nbuckets + 1));
    ix->items = malloc(sizeof(mm_item) * (total ? total : 1));
    ix->place_items = malloc(sizeof(int32_t) * (total_places ? total_places : 1));
    if (!ix->bucket_start || !ix->place_start || !ix->items || !ix->place_items)
        goto fail;

    int32_t acc = 0;
    for (size_t i = 0; i <= nbuckets; i++) {
        ix->bucket_start[i] = acc;
        if (i < nbuckets)
            acc += counts[i];
    }
    acc = 0;
    for (size_t i = 0; i <= nbuckets; i++) {
        ix->place_start[i] = acc;
        if (i < nbuckets)
            acc += place_counts[i];
    }

    memset(counts, 0, sizeof(int32_t) * (nbuckets + 1));
    memset(place_counts, 0, sizeof(int32_t) * (nbuckets + 1));
    ctx.counts = counts;
    ctx.items = ix->items;
    ctx.seq = 0;
    for_each_rendered_way(ix, fill_cb, &ctx);
    for (int32_t i = 0; i < d->n_places; i++) {
        int32_t b = place_bucket(ix, i);
        if (b >= 0)
            ix->place_items[ix->place_start[b] + place_counts[b]++] = i;
    }
    free(counts);
    free(place_counts);
    counts = place_counts = NULL;

    /* water, railway, roads minor -> major (stable inside a class) */
    for (size_t b = 0; b < nbuckets; b++) {
        int32_t s = ix->bucket_start[b], e = ix->bucket_start[b + 1];
        if (e - s > 1)
            qsort(ix->items + s, (size_t)(e - s), sizeof(mm_item), cmp_item);
    }
    return ix;

fail:
    free(counts);
    free(place_counts);
    mm_index_free(ix);
    return NULL;
}

void mm_index_free(mm_index *ix)
{
    if (!ix)
        return;
    free(ix->items);
    free(ix->bucket_start);
    free(ix->place_start);
    free(ix->place_items);
    free(ix);
}

/* ----------------------------------------------------------- raster ops -- */

static inline void put_px(uint8_t *img, int x, int y, uint8_t v)
{
    if ((unsigned)x < TILE_PX && (unsigned)y < TILE_PX)
        img[y * TILE_PX + x] = v;
}

/* Bresenham: identical to the classic thin-line rasterisation. */
static void draw_thin_line(uint8_t *img, int x0, int y0, int x1, int y1, uint8_t v)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(img, x0, y0, v);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_disk(uint8_t *img, double cx, double cy, double r, uint8_t v)
{
    /* pixel centres are at integer coordinates (PIL convention) */
    int x0 = (int)floor(cx - r), x1 = (int)ceil(cx + r);
    int y0 = (int)floor(cy - r), y1 = (int)ceil(cy + r);
    double r2 = r * r;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double dx = (double)x - cx, dy = (double)y - cy;
            if (dx * dx + dy * dy <= r2)
                put_px(img, x, y, v);
        }
}

/* Filled band of one segment.
 *
 * The band is built by offsetting the segment by +/- w/2 along the
 * perpendicular and filling the resulting quad with the integer pixel-centre
 * scanline convention. This was calibrated against Pillow's wide-line
 * rasteriser (see tools/tests/test_native.py): it reproduces the same ink
 * coverage to within ~2% and the same pixel image to within ~3% on dense
 * maps, the differences being single-pixel edges. */
static void fill_quad(uint8_t *img, const double *qx, const double *qy, uint8_t v)
{
    double ymin_d = qy[0], ymax_d = qy[0];
    for (int i = 1; i < 4; i++) {
        if (qy[i] < ymin_d) ymin_d = qy[i];
        if (qy[i] > ymax_d) ymax_d = qy[i];
    }
    int ymin = (int)floor(ymin_d), ymax = (int)ceil(ymax_d);
    if (ymin < 0) ymin = 0;
    if (ymax > TILE_PX - 1) ymax = TILE_PX - 1;
    for (int y = ymin; y <= ymax; y++) {
        double yc = y;
        double xs[4];
        int n = 0;
        for (int i = 0; i < 4; i++) {
            double x0 = qx[i], y0 = qy[i], x1 = qx[(i + 1) & 3], y1 = qy[(i + 1) & 3];
            if (y0 == y1)
                continue;
            if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) {
                double t = (yc - y0) / (y1 - y0);
                xs[n++] = x0 + t * (x1 - x0);
            }
        }
        if (n < 2)
            continue;
        if (n == 2) {
            if (xs[0] > xs[1]) { double tmp = xs[0]; xs[0] = xs[1]; xs[1] = tmp; }
        } else {
            for (int i = 1; i < n; i++) {
                double key = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > key) {
                    xs[j + 1] = xs[j];
                    j--;
                }
                xs[j + 1] = key;
            }
        }
        for (int i = 0; i + 1 < n; i += 2) {
            int a = (int)ceil(xs[i]);
            int b = (int)ceil(xs[i + 1]) - 1;
            if (a < 0) a = 0;
            if (b > TILE_PX - 1) b = TILE_PX - 1;
            uint8_t *row = img + (size_t)y * TILE_PX;
            for (int x = a; x <= b; x++)
                row[x] = v;
        }
    }
}

static void draw_band(uint8_t *img, double x0, double y0, double x1, double y1,
                      int w, uint8_t v)
{
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-9) {
        draw_disk(img, x0, y0, w / 2.0, v);
        return;
    }
    double ux = dx / len, uy = dy / len;
    double nx = -uy, ny = ux;
    int a = w / 2, b = w / 2;
    /* offset band: o in [-a, +b] along the perpendicular direction */
    double qx[4] = {x0 + nx * b, x1 + nx * b, x1 - nx * a, x0 - nx * a};
    double qy[4] = {y0 + ny * b, y1 + ny * b, y1 - ny * a, y0 - ny * a};
    fill_quad(img, qx, qy, v);
}

/* Polyline stroke: butt caps + round joins (PIL "joint=curve" look). */
static void stroke_polyline(uint8_t *img, const int *px, const int *py, int n,
                            int width, uint8_t v)
{
    if (n < 2)
        return;
    if (width <= 1) {
        for (int i = 0; i + 1 < n; i++) {
            /* cheap rejection: segments that cannot touch this tile */
            if ((px[i] < -1 && px[i + 1] < -1) || (px[i] > TILE_PX && px[i + 1] > TILE_PX) ||
                (py[i] < -1 && py[i + 1] < -1) || (py[i] > TILE_PX && py[i + 1] > TILE_PX))
                continue;
            draw_thin_line(img, px[i], py[i], px[i + 1], py[i + 1], v);
        }
        return;
    }
    for (int i = 0; i + 1 < n; i++)
        draw_band(img, px[i], py[i], px[i + 1], py[i + 1], width, v);
    double jr = (double)(width / 2);
    for (int i = 1; i + 1 < n; i++)
        draw_disk(img, px[i], py[i], jr, v);
}

/* Even-odd scanline fill, evaluated on the integer pixel-centre grid (matches
 * Pillow's polygon rasterisation convention). */
static void fill_polygon(uint8_t *img, const int *px, const int *py, int n, uint8_t v)
{
    if (n < 3)
        return;
    double *xs = malloc(sizeof(double) * (size_t)n);
    if (!xs)
        return;
    int ymin = py[0], ymax = py[0];
    for (int i = 1; i < n; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    if (ymin < 0) ymin = 0;
    if (ymax > TILE_PX - 1) ymax = TILE_PX - 1;
    for (int y = ymin; y <= ymax; y++) {
        double yc = y;
        int nx = 0;
        for (int i = 0; i < n; i++) {
            double x0 = px[i], y0 = py[i];
            double x1 = px[(i + 1) % n], y1 = py[(i + 1) % n];
            if (y0 == y1)
                continue;
            if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) {
                double t = (yc - y0) / (y1 - y0);
                xs[nx++] = x0 + t * (x1 - x0);
            }
        }
        for (int i = 1; i < nx; i++) { /* insertion sort */
            double key = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > key) {
                xs[j + 1] = xs[j];
                j--;
            }
            xs[j + 1] = key;
        }
        for (int i = 0; i + 1 < nx; i += 2) {
            int a = (int)ceil(xs[i]);
            int b = (int)ceil(xs[i + 1]) - 1;
            if (a < 0) a = 0;
            if (b > TILE_PX - 1) b = TILE_PX - 1;
            for (int x = a; x <= b; x++)
                img[y * TILE_PX + x] = v;
        }
    }
    free(xs);
}

/* --------------------------------------------------------------- labels -- */

typedef struct {
    uint32_t offset;
    uint16_t len;
} atlas_entry;

static atlas_entry *g_atlas_index;
static const unsigned char *g_atlas_data;
static int atlas_init(void);

/* Font variant / dot radius for a place kind (mirrors label_font() in Python). */
static int label_font_for(int kind, int z, int *variant, int *dot_r)
{
    if (kind == MM_PLACE_CITY) {
        *variant = MM_FONT_CITY;
        *dot_r = 4;
        return 1;
    }
    if (kind == MM_PLACE_TOWN) {
        *variant = MM_FONT_TOWN;
        *dot_r = 3;
        return 1;
    }
    if ((kind == MM_PLACE_VILLAGE || kind == MM_PLACE_SUBURB) && z >= 13) {
        *variant = MM_FONT_VILLAGE;
        *dot_r = 2;
        return 1;
    }
    if ((kind == MM_PLACE_HAMLET || kind == MM_PLACE_NEIGHBOURHOOD) && z >= 14) {
        *variant = MM_FONT_HAMLET;
        *dot_r = 2;
        return 1;
    }
    return 0;
}

/* Advance width and vertical ink extent of a string, from the glyph atlas. */
static void text_metrics(const char *text, int variant, int *width, int *y_top, int *y_bottom)
{
    const int n_codes = MONOMESH_ATLAS_LAST_CHAR - MONOMESH_ATLAS_FIRST_CHAR + 1;
    int w = 0, top = 0, bottom = 0, first = 1;
    if (atlas_init() != 0) {
        *width = 0;
        *y_top = 0;
        *y_bottom = 0;
        return;
    }
    for (const unsigned char *s = (const unsigned char *)text; *s; s++) {
        int code = *s;
        if (code < MONOMESH_ATLAS_FIRST_CHAR || code > MONOMESH_ATLAS_LAST_CHAR)
            code = '?';
        const atlas_entry *e =
            &g_atlas_index[variant * n_codes + (code - MONOMESH_ATLAS_FIRST_CHAR)];
        const unsigned char *g = g_atlas_data + e->offset;
        int gh = g[1];
        int gtop = (int8_t)g[3];
        int advance = (int16_t)(g[4] | (g[5] << 8));
        if (gh > 0) {
            if (first || gtop < top)
                top = gtop;
            if (first || gtop + gh > bottom)
                bottom = gtop + gh;
            first = 0;
        }
        w += advance;
    }
    *width = (w + 32) >> 6;
    *y_top = top;
    *y_bottom = bottom;
}

typedef struct {
    int score;
    int32_t place;
} label_cand;

static int cmp_label_cand(const void *a, const void *b)
{
    const label_cand *x = a, *y = b;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;   /* higher score first */
    return x->place < y->place ? -1 : (x->place > y->place ? 1 : 0);
}

typedef struct {
    int x0, y0, x1, y1;
} label_box;

static int boxes_overlap(const label_box *a, const label_box *b)
{
    const int pad = 1;
    return !(a->x1 + pad <= b->x0 || b->x1 + pad <= a->x0 ||
             a->y1 + pad <= b->y0 || b->y1 + pad <= a->y0);
}

static int atlas_init(void)
{
    if (g_atlas_index)
        return 0;
    const int n_codes = MONOMESH_ATLAS_LAST_CHAR - MONOMESH_ATLAS_FIRST_CHAR + 1;
    g_atlas_index = malloc(sizeof(atlas_entry) * (size_t)n_codes * MONOMESH_ATLAS_VARIANTS);
    if (!g_atlas_index)
        return -1;
    g_atlas_data = monomesh_glyph_atlas;
    uint32_t pos = 0;
    for (int v = 0; v < MONOMESH_ATLAS_VARIANTS; v++) {
        for (int c = 0; c < n_codes; c++) {
            atlas_entry *e = &g_atlas_index[v * n_codes + c];
            if (pos + 6 > monomesh_glyph_atlas_len)
                return -1;
            unsigned w = g_atlas_data[pos], h = g_atlas_data[pos + 1];
            uint32_t plen = 6u + (uint32_t)w * h;
            e->offset = pos;
            e->len = (uint16_t)plen;
            pos += plen;
        }
    }
    return 0;
}

/* Draws one text run: halo pass (background) then glyphs, like PIL
 * draw.text(..., stroke_width=2, stroke_fill=background). */
static void draw_text(uint8_t *img, int x, int y, const char *text, int variant,
                      uint8_t fg, uint8_t bg)
{
    if (atlas_init() != 0 || !text)
        return;
    const int n_codes = MONOMESH_ATLAS_LAST_CHAR - MONOMESH_ATLAS_FIRST_CHAR + 1;

    for (int pass = 0; pass < 2; pass++) {
        int64_t pen = (int64_t)x << 6; /* 26.6 fixed point, like the atlas */
        for (const unsigned char *s = (const unsigned char *)text; *s; s++) {
            int code = *s;
            if (code < MONOMESH_ATLAS_FIRST_CHAR || code > MONOMESH_ATLAS_LAST_CHAR)
                code = '?';
            const atlas_entry *e =
                &g_atlas_index[variant * n_codes + (code - MONOMESH_ATLAS_FIRST_CHAR)];
            const unsigned char *g = g_atlas_data + e->offset;
            int w = g[0], h = g[1];
            int left = (int8_t)g[2], top = (int8_t)g[3];
            int advance = (int16_t)(g[4] | (g[5] << 8));
            const unsigned char *cov = g + 6;
            int gx = (int)((pen + 32) >> 6) + left, gy = y + top;

            if (pass == 0) {
                for (int yy = -2; yy < h + 2; yy++) {
                    for (int xx = -2; xx < w + 2; xx++) {
                        int hit = 0;
                        for (int dy = -2; dy <= 2 && !hit; dy++) {
                            int sy = yy + dy;
                            if (sy < 0 || sy >= h)
                                continue;
                            for (int dx = -2; dx <= 2; dx++) {
                                int sx = xx + dx;
                                if (sx < 0 || sx >= w)
                                    continue;
                                if (dx * dx + dy * dy > 5)
                                    continue;
                                if (cov[sy * w + sx] >= 128) {
                                    hit = 1;
                                    break;
                                }
                            }
                        }
                        if (hit)
                            put_px(img, gx + xx, gy + yy, bg);
                    }
                }
            } else {
                for (int yy = 0; yy < h; yy++) {
                    for (int xx = 0; xx < w; xx++) {
                        int c = cov[yy * w + xx];
                        if (!c)
                            continue;
                        int px = gx + xx, py = gy + yy;
                        if ((unsigned)px >= TILE_PX || (unsigned)py >= TILE_PX)
                            continue;
                        if (c >= 255) {
                            img[py * TILE_PX + px] = fg;
                        } else {
                            int old = img[py * TILE_PX + px];
                            img[py * TILE_PX + px] =
                                (uint8_t)((fg * c + old * (255 - c)) / 255);
                        }
                    }
                }
            }
            pen += advance;
        }
    }
}

/* ------------------------------------------------------------ PNG writer -- */

static void be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int png_chunk(FILE *f, const char *type, const unsigned char *data, size_t len)
{
    unsigned char hdr[8], crcbuf[4];
    be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8)
        return -1;
    if (len && fwrite(data, 1, len, f) != len)
        return -1;
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len)
        crc = crc32(crc, data, (uInt)len);
    be32(crcbuf, (uint32_t)crc);
    return fwrite(crcbuf, 1, 4, f) == 4 ? 0 : -1;
}

static int png_write(const char *path, const uint8_t *img, int bits, size_t *out_size)
{
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    int row_bytes = (bits == 1) ? (TILE_PX / 8) : TILE_PX;
    size_t raw_len = (size_t)(row_bytes + 1) * TILE_PX;
    unsigned char *raw = malloc(raw_len);
    if (!raw)
        return -1;
    for (int y = 0; y < TILE_PX; y++) {
        unsigned char *row = raw + (size_t)y * (row_bytes + 1);
        row[0] = 0; /* filter: none */
        if (bits == 1) {
            memset(row + 1, 0, (size_t)row_bytes);
            for (int x = 0; x < TILE_PX; x++)
                if (img[y * TILE_PX + x] >= 128)
                    row[1 + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
        } else {
            memcpy(row + 1, img + (size_t)y * TILE_PX, TILE_PX);
        }
    }
    uLongf clen = compressBound((uLong)raw_len);
    unsigned char *comp = malloc(clen);
    if (!comp) {
        free(raw);
        return -1;
    }
    if (compress2(comp, &clen, raw, (uLong)raw_len, 6) != Z_OK) {
        free(raw);
        free(comp);
        return -1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(comp);
        return -1;
    }
    unsigned char ihdr[13];
    be32(ihdr, TILE_PX);
    be32(ihdr + 4, TILE_PX);
    ihdr[8] = (unsigned char)bits;
    ihdr[9] = 0;  /* greyscale */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    int ok = fwrite(sig, 1, 8, f) == 8
             && png_chunk(f, "IHDR", ihdr, 13) == 0
             && png_chunk(f, "IDAT", comp, clen) == 0
             && png_chunk(f, "IEND", NULL, 0) == 0;
    fclose(f);
    free(comp);
    if (!ok)
        return -1;
    if (out_size) {
        *out_size = 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- render -- */

void mm_render_tile_img(mm_index *ix, int z, int tx, int ty, int style_dark, uint8_t *img)
{
    uint8_t bg = style_dark ? 0 : 255;
    uint8_t fg = style_dark ? 255 : 0;
    const mm_data *d = ix->data;
    memset(img, bg, (size_t)TILE_PX * TILE_PX);

    if (tx < ix->grid.tx0 || tx >= ix->grid.tx0 + ix->grid.nx ||
        ty < ix->grid.ty0 || ty >= ix->grid.ty0 + ix->grid.ny)
        return;
    size_t b = (size_t)(ty - ix->grid.ty0) * ix->grid.nx + (tx - ix->grid.tx0);
    int32_t s = ix->bucket_start[b], e = ix->bucket_start[b + 1];

    int max_pts = 16;
    for (int32_t i = s; i < e; i++) {
        int32_t wp = d->ways[ix->items[i].way].npts;
        if (wp > max_pts)
            max_pts = wp;
    }
    int *px = malloc(sizeof(int) * (size_t)max_pts);
    int *py = malloc(sizeof(int) * (size_t)max_pts);
    if (!px || !py) {
        free(px);
        free(py);
        return;
    }

    for (int32_t i = s; i < e; i++) {
        int32_t wi = ix->items[i].way;
        const mm_way *way = &d->ways[wi];
        int prio, width, casing;
        if (!way_style(way, z, &prio, &width, &casing))
            continue;
        int n = way->npts;
        if (n < 2)
            continue;
        for (int j = 0; j < n; j++) {
            double x = ix->xy[2 * (way->off + j)] * ix->scale;
            double y = ix->xy[2 * (way->off + j) + 1] * ix->scale;
            px[j] = (int)lround((x - tx) * 256.0);
            py[j] = (int)lround((y - ty) * 256.0);
        }
        if (way->type == MM_WATER) {
            uint8_t color = (way->prio == MM_WATER_RING_INNER) ? bg : fg;
            if (width == 0) {
                if (n >= 3)
                    fill_polygon(img, px, py, n, color);
            } else {
                stroke_polyline(img, px, py, n, width, color);
            }
        } else if (way->type == MM_RAILWAY) {
            stroke_polyline(img, px, py, n, 1, fg);
        } else {
            stroke_polyline(img, px, py, n, width, fg);
            if (casing && width >= 3)
                stroke_polyline(img, px, py, n, width - 2, bg);
        }
    }

    /* Labels: most important first, within a per tile budget and without
     * overlaps - like the reference style (otherwise low zoom tiles end up
     * covered in names). */
    int32_t ps = ix->place_start[b], pe = ix->place_start[b + 1];
    int n_candidates = pe - ps;
    if (n_candidates > 0) {
        label_cand *cands = malloc(sizeof(label_cand) * (size_t)n_candidates);
        int nc = 0;
        if (cands) {
            for (int32_t i = ps; i < pe; i++) {
                int32_t pi = ix->place_items[i];
                int kind = d->place_kind[pi];
                if (!d->name_len[pi] || !mm_label_kind_drawn(kind, z))
                    continue;
                int variant, dot_r;
                if (!label_font_for(kind, z, &variant, &dot_r))
                    continue;
                cands[nc].score = mm_label_score(kind, d->place_population[pi]);
                cands[nc].place = pi;
                nc++;
            }
            qsort(cands, (size_t)nc, sizeof(label_cand), cmp_label_cand);
        }
        label_box placed[24];
        int n_placed = 0;
        int budget = mm_label_budget(z);
        if (budget > 24)
            budget = 24;
        double n2 = pow(2.0, z);
        for (int i = 0; i < nc && n_placed < budget; i++) {
            int32_t pi = cands[i].place;
            int kind = d->place_kind[pi];
            int variant, dot_r;
            if (!label_font_for(kind, z, &variant, &dot_r))
                continue;
            int32_t no = d->name_off[pi];
            uint16_t nl = d->name_len[pi];
            char name[256];
            size_t copy = nl < sizeof(name) - 1 ? nl : sizeof(name) - 1;
            memcpy(name, d->names + no, copy);
            name[copy] = '\0';
            double lat = d->place_lat_e5[pi] / MM_E5;
            double lon = d->place_lon_e5[pi] / MM_E5;
            int cx = (int)lround(((lon + 180.0) / 360.0 * n2 - tx) * 256.0);
            int cy = (int)lround(((1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * n2 - ty) * 256.0);
            int x = cx + dot_r + 3;
            int y = cy - 6;
            int tw, ytop, ybottom;
            text_metrics(name, variant, &tw, &ytop, &ybottom);
            label_box box = {x, y + ytop - 2, x + tw, y + ybottom + 2};
            if (box.x1 < 0 || box.x0 > TILE_PX || box.y1 < 0 || box.y0 > TILE_PX)
                continue;
            int clash = 0;
            for (int k = 0; k < n_placed; k++)
                if (boxes_overlap(&box, &placed[k])) {
                    clash = 1;
                    break;
                }
            if (clash)
                continue;
            placed[n_placed++] = box;
            draw_disk(img, cx, cy, dot_r, fg);
            draw_text(img, x, y, name, variant, fg, bg);
        }
        free(cands);
    }

    free(px);
    free(py);
}

int mm_render_tile_png(mm_index *ix, int z, int tx, int ty, int style_dark,
                       int png_bits, const char *out_path, size_t *out_size)
{
    uint8_t *img = malloc((size_t)TILE_PX * TILE_PX);
    if (!img)
        return -1;
    mm_render_tile_img(ix, z, tx, ty, style_dark, img);
    int rc = png_write(out_path, img, png_bits, out_size);
    free(img);
    return rc;
}
