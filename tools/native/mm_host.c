/* Host test driver for libmapcore: renders viewports from a v2 container,
 * produces reference renders from the old tile pipeline, benchmarks and
 * fuzzes the reader.  Used by tools/tests/test_libmapcore.py.
 *
 *   mm_host render   --input B.mmap --lat --lon --zoom --width --height --out X.pgm
 *   mm_host refrender --cache C.mosmb --lat --lon --zoom --width --height --out X.pgm
 *   mm_host fuzzread --input B.mmap [--deep]
 *   mm_host bench    --input B.mmap --lat --lon --zoom --frames N
 */
#include "monomesh.h"

#include "libmapcore/mm_core.h"
#include "libmapcore/mm_inflate.h"
#include "libmapcore/mm_raster.h"
#include "libmapcore/mm_render.h"
#include "libmapcore/mm_text.h"
#include "libmapcore/mm_vmap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ file io ---- */

typedef struct {
    FILE *f;
} file_ctx;

static int64_t file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    file_ctx *c = (file_ctx *)ctx;
    if (fseeko(c->f, (off_t)off, SEEK_SET) != 0)
        return -1;
    return (int64_t)fread(buf, 1, len, c->f);
}

/* Byte budgeted LRU of decompressed chunks: mimics the device cache (budget
 * D6 revised: 1 MB of decompressed payloads).  Slot counting thrashes - a z12 screen
 * touches ~60 small M chunks - so the ring is limited by bytes, not entries. */
#define CACHE_MAX_BYTES (1024 * 1024)
#define CACHE_MAX_ENTRIES 256
typedef struct {
    int band;
    uint32_t cx, cy;
    uint8_t *data;
    size_t len;
    uint16_t flags;
    uint32_t stamp;
    mm_chunk_dir dir;
} slot;

typedef struct {
    uint32_t off, len;
    uint16_t flags;
} lcell;

typedef struct {
    mm_vmap *map;
    slot slots[CACHE_MAX_ENTRIES];
    int n;
    uint32_t clock;
    size_t bytes;
    uint8_t *scratch;
    size_t scratch_cap;
    uint64_t hits, misses, io_bytes;
    /* pinned overview band (band 0), like the device: every z7-11 view reads
     * most of L, a ring cannot hold it */
    uint8_t *lpin;
    size_t lpin_len, lpin_cap;
    lcell *lcells;
    mm_chunk_dir *ldirs;
    uint32_t lcount;
} chunk_cache;

/* Host tools have no PSRAM budget: build the full directory. */
#define DIR_MAX_RECORDS (1u << 20)

static void *dir_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void dir_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static int cache_pin_band0(chunk_cache *cc)
{
    if (cc->lpin)
        return 0;
    const mm_vmap_band *b = &cc->map->bands[0];
    cc->lcount = b->nx * b->ny;
    cc->lcells = (lcell *)calloc(cc->lcount, sizeof(lcell));
    cc->ldirs = (mm_chunk_dir *)calloc(cc->lcount, sizeof(mm_chunk_dir));
    if (!cc->lcells || !cc->ldirs)
        return -1;
    cc->lpin_cap = (size_t)b->payload_bytes + 256 * 1024;
    cc->lpin = (uint8_t *)malloc(cc->lpin_cap);
    if (!cc->lpin)
        return -1;
    for (uint32_t cy = 0; cy < b->ny; cy++) {
        for (uint32_t cx = 0; cx < b->nx; cx++) {
            uint32_t idx = cy * b->nx + cx;
            for (int attempt = 0; attempt < 8; attempt++) {
                size_t got = 0;
                uint16_t fl = 0;
                int rc = mm_vmap_read_chunk(cc->map, 0, b->x0 + cx, b->y0 + cy,
                                            cc->lpin + cc->lpin_len,
                                            cc->lpin_cap - cc->lpin_len, &got, &fl);
                if (rc > 0)
                    break;  /* empty cell: nothing to pin */
                if (rc == 0) {
                    cc->lcells[idx].off = (uint32_t)cc->lpin_len;
                    cc->lcells[idx].len = (uint32_t)got;
                    cc->lcells[idx].flags = fl;
                    cc->lpin_len += got;
                    cc->misses++;
                    cc->io_bytes += got;
                    break;
                }
                /* arena full: grow and retry */
                size_t want = cc->lpin_cap * 2;
                uint8_t *bigger = (uint8_t *)realloc(cc->lpin, want);
                if (!bigger)
                    return -1;
                cc->lpin = bigger;
                cc->lpin_cap = want;
            }
        }
    }
    return 0;
}

static void cache_evict(chunk_cache *cc, size_t need)
{
    while (cc->bytes + need > CACHE_MAX_BYTES || cc->n >= CACHE_MAX_ENTRIES) {
        int victim = -1;
        for (int i = 0; i < cc->n; i++)
            if (victim < 0 || cc->slots[i].stamp < cc->slots[victim].stamp)
                victim = i;
        if (victim < 0)
            return;
        free(cc->slots[victim].data);
        mm_chunk_dir_free(&cc->slots[victim].dir, dir_free, NULL);
        cc->bytes -= cc->slots[victim].len;
        cc->slots[victim] = cc->slots[cc->n - 1];
        cc->n--;
    }
}

static int cache_get(void *user, int band, uint32_t cx, uint32_t cy,
                     const uint8_t **data, size_t *len, uint16_t *flags)
{
    chunk_cache *cc = (chunk_cache *)user;
    cc->clock++;
    if (band == 0) {
        if (cache_pin_band0(cc) != 0)
            return -1;
        const mm_vmap_band *b = &cc->map->bands[0];
        if (cx < b->x0 || cy < b->y0 || cx >= b->x0 + b->nx || cy >= b->y0 + b->ny)
            return 1;
        const lcell *c = &cc->lcells[(cy - b->y0) * b->nx + (cx - b->x0)];
        if (c->len == 0)
            return 1;
        *data = cc->lpin + c->off;
        *len = c->len;
        *flags = c->flags;
        cc->hits++;
        return 0;
    }
    for (int i = 0; i < cc->n; i++) {
        if (cc->slots[i].band == band && cc->slots[i].cx == cx && cc->slots[i].cy == cy) {
            cc->slots[i].stamp = cc->clock;
            cc->hits++;
            *data = cc->slots[i].data;
            *len = cc->slots[i].len;
            *flags = cc->slots[i].flags;
            return 0;
        }
    }
    /* growable inflate scratch: an overview chunk can be ~850 KB decompressed */
    if (!cc->scratch) {
        cc->scratch_cap = 1024 * 1024;
        cc->scratch = (uint8_t *)malloc(cc->scratch_cap);
        if (!cc->scratch)
            return -1;
    }
    size_t out_len = 0;
    uint16_t cflags = 0;
    int rc = -1;
    for (int attempt = 0; attempt < 4; ++attempt) {
        rc = mm_vmap_read_chunk(cc->map, band, cx, cy, cc->scratch, cc->scratch_cap,
                                &out_len, &cflags);
        if (rc == 0)
            break;
        if (rc < 0 && cc->scratch_cap < 4 * 1024 * 1024) {
            cc->scratch_cap *= 2;
            uint8_t *bigger = (uint8_t *)realloc(cc->scratch, cc->scratch_cap);
            if (!bigger)
                return -1;
            cc->scratch = bigger;
            continue;
        }
        return rc > 0 ? 1 : -1;
    }
    uint8_t *tmp = cc->scratch;
    cache_evict(cc, out_len);
    uint8_t *buf = (uint8_t *)malloc(out_len ? out_len : 1);
    if (!buf)
        return -1;
    memcpy(buf, tmp, out_len);
    cc->slots[cc->n++] = (slot){band, cx, cy, buf, out_len, cflags, cc->clock,
                                {NULL, NULL, 0, 0}};
    cc->bytes += out_len;
    cc->misses++;
    cc->io_bytes += out_len;
    *data = buf;
    *len = out_len;
    *flags = cflags;
    return 0;
}

/* Record directory for the same cell cache_get() just served: built once per
 * chunk (band 0) or per ring slot and reused by every pass and every strip. */
static int cache_get_dir(void *user, int band, uint32_t cx, uint32_t cy,
                         const mm_chunk_dir **dir)
{
    chunk_cache *cc = (chunk_cache *)user;
    if (band == 0) {
        if (cache_pin_band0(cc) != 0)
            return -1;
        const mm_vmap_band *b = &cc->map->bands[0];
        if (cx < b->x0 || cy < b->y0 || cx >= b->x0 + b->nx || cy >= b->y0 + b->ny)
            return 1;
        uint32_t idx = (cy - b->y0) * b->nx + (cx - b->x0);
        const lcell *c = &cc->lcells[idx];
        if (c->len == 0 || !cc->ldirs)
            return 1;
        mm_chunk_dir *d = &cc->ldirs[idx];
        if (!d->pos) {
            if (mm_chunk_dir_build(d, cc->lpin + c->off, c->len, DIR_MAX_RECORDS,
                                   dir_alloc, dir_free, NULL) != 0)
                return 1; /* OOM: legacy walk for this cell */
        }
        *dir = d;
        return 0;
    }
    for (int i = 0; i < cc->n; i++) {
        slot *s = &cc->slots[i];
        if (s->band == band && s->cx == cx && s->cy == cy) {
            if (!s->dir.pos) {
                if (mm_chunk_dir_build(&s->dir, s->data, s->len, DIR_MAX_RECORDS,
                                       dir_alloc, dir_free, NULL) != 0)
                    return 1; /* OOM: legacy walk for this cell */
            }
            *dir = &s->dir;
            return 0;
        }
    }
    return -1;
}

static void cache_free(chunk_cache *cc)
{
    for (int i = 0; i < cc->n; i++) {
        free(cc->slots[i].data);
        mm_chunk_dir_free(&cc->slots[i].dir, dir_free, NULL);
    }
    if (cc->ldirs) {
        for (uint32_t i = 0; i < cc->lcount; i++)
            mm_chunk_dir_free(&cc->ldirs[i], dir_free, NULL);
        free(cc->ldirs);
    }
    free(cc->scratch);
    free(cc->lpin);
    free(cc->lcells);
    memset(cc, 0, sizeof(*cc));
}

static int write_pgm(const char *path, const uint8_t *pix, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    size_t n = fwrite(pix, 1, (size_t)w * h, f);
    fclose(f);
    return n == (size_t)w * h ? 0 : -1;
}

/* ------------------------------------------------------------- render ---- */

static int cmd_render(const char *path, double lat, double lon, double z,
                      int w, int h, const char *out, int dark, int no_labels,
                      int no_dirs)
{
    file_ctx ctx;
    ctx.f = fopen(path, "rb");
    if (!ctx.f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseeko(ctx.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(ctx.f);
    mm_vmap_io io = {file_read, &ctx, size};
    mm_vmap map;
    if (mm_vmap_open(&map, &io) != 0) {
        fprintf(stderr, "refused\n");
        fclose(ctx.f);
        return 1;
    }
    uint8_t *pix = (uint8_t *)malloc((size_t)w * h);
    void *scratch = malloc(1 << 18);
    if (!pix || !scratch) {
        fprintf(stderr, "oom\n");
        return 1;
    }
    mm_canvas canvas = {pix, w, h, w,
                        {0, 0, w * MM_FP_ONE - 1, h * MM_FP_ONE - 1}, scratch, 1 << 18};
    mm_theme theme = {dark ? 255 : 0, dark ? 0 : 255};
    mm_canvas_clear(&canvas, theme.bg);

    chunk_cache cc;
    memset(&cc, 0, sizeof(cc));
    cc.map = &map;
    mm_chunks src = {cache_get, cache_get_dir, &cc};
    if (no_dirs)
        src.get_dir = NULL; /* legacy walk: used to prove the cull is bit exact */
    mm_view view;
    view.z = z;
    view.cx = (lon + 180.0) / 360.0 * pow(2.0, z) * 256.0;
    view.cy = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * pow(2.0, z) * 256.0;
    view.width = w;
    view.height = h;

    if (no_labels) {
        map.has_poigrid = 0; /* golden pixel diffs run without labels */
        map.poi_count = 0;
    }
    if (mm_render_view(&map, &src, &view, &canvas, &theme) != 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }
    int rc = write_pgm(out, pix, w, h);
    fprintf(stderr, "render ok (%d chunk reads, %d hits)\n", (int)cc.misses,
            (int)cc.hits);
    cache_free(&cc);
    free(pix);
    free(scratch);
    fclose(ctx.f);
    return rc;
}

/* ------------------------------------------------------ label cache test --- */

static int pix_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    int d = 0;
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            d++;
    return d;
}

/* Exercises the D5 label cache end to end:
 *  1. a fresh cache must render exactly like an uncached render;
 *  2. rendering the same viewport twice must not rebuild the layout;
 *  3. a pan inside the slack reuses the layout (no rebuild);
 *  4. a pan beyond the slack rebuilds and matches the uncached render again. */
static int cmd_cachetest(const char *path, double lat, double lon, double z, int w,
                         int h, double panx, double pany, const char *out,
                         const char *out2, int dark, int block_rebuild)
{
    file_ctx ctx;
    ctx.f = fopen(path, "rb");
    if (!ctx.f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseeko(ctx.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(ctx.f);
    mm_vmap_io io = {file_read, &ctx, size};
    mm_vmap map;
    if (mm_vmap_open(&map, &io) != 0) {
        fclose(ctx.f);
        return 1;
    }
    size_t npix = (size_t)w * h;
    uint8_t *pixA = (uint8_t *)malloc(npix);
    uint8_t *pixB = (uint8_t *)malloc(npix);
    void *scratchA = malloc(1 << 18);
    void *scratchB = malloc(1 << 18);
    if (!pixA || !pixB || !scratchA || !scratchB) {
        fprintf(stderr, "oom\n");
        return 1;
    }
    chunk_cache ccA, ccB;
    memset(&ccA, 0, sizeof(ccA));
    memset(&ccB, 0, sizeof(ccB));
    ccA.map = &map;
    ccB.map = &map;
    mm_chunks srcA = {cache_get, cache_get_dir, &ccA};
    mm_chunks srcB = {cache_get, cache_get_dir, &ccB};
    mm_theme theme = {dark ? 255 : 0, dark ? 0 : 255};

    mm_view view;
    view.z = z;
    view.cx = (lon + 180.0) / 360.0 * pow(2.0, z) * 256.0;
    view.cy = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * pow(2.0, z) * 256.0;
    view.width = w;
    view.height = h;

    mm_canvas ca = {pixA, w, h, w, {0, 0, w * MM_FP_ONE - 1, h * MM_FP_ONE - 1},
                    scratchA, 1 << 18};
    mm_canvas cb = {pixB, w, h, w, {0, 0, w * MM_FP_ONE - 1, h * MM_FP_ONE - 1},
                    scratchB, 1 << 18};
    mm_canvas_clear(&ca, theme.bg);
    if (mm_render_view(&map, &srcA, &view, &ca, &theme) != 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }
    if (out && write_pgm(out, pixA, w, h) != 0) return 1;

    mm_label_cache cache;
    memset(&cache, 0, sizeof(cache));
    mm_canvas_clear(&cb, theme.bg);
    if (mm_render_view_cached(&map, &srcB, &view, &cb, &theme, &cache) != 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }
    printf("cache fresh mismatches: %d\n", pix_diff(pixA, pixB, npix));

    uint32_t r0 = cache.rebuilds;
    mm_canvas_clear(&cb, theme.bg);
    mm_render_view_cached(&map, &srcB, &view, &cb, &theme, &cache);
    printf("cache repeat rebuilds: %u\n", (unsigned)(cache.rebuilds - r0));

    mm_view panned = view;
    panned.cx += panx;
    panned.cy += pany;
    r0 = cache.rebuilds;
    mm_canvas_clear(&cb, theme.bg);
    if (block_rebuild)
        mm_render_view_cached_ex(&map, &srcB, &panned, &cb, &theme, &cache, 0);
    else
        mm_render_view_cached(&map, &srcB, &panned, &cb, &theme, &cache);
    printf("cache pan(%.0f,%.0f) rebuilds: %u labels: %d\n", panx, pany,
           (unsigned)(cache.rebuilds - r0), cache.n);
    if (out2 && write_pgm(out2, pixB, w, h) != 0) return 1;

    mm_canvas_clear(&ca, theme.bg);
    mm_render_view(&map, &srcA, &panned, &ca, &theme);
    printf("pan uncached mismatches: %d\n", pix_diff(pixA, pixB, npix));

    cache_free(&ccA);
    cache_free(&ccB);
    free(pixA);
    free(pixB);
    free(scratchA);
    free(scratchB);
    fclose(ctx.f);
    return 0;
}

/* -------------------------------------------------------------- stripcheck --- */

/* A strip repaint must write exactly the pixels a full render writes inside
 * the clip, and nothing outside it.  This catches both clip bugs and a cell
 * range too narrow for the records that overhang the strip. */
static int cmd_stripcheck(const char *path, double lat, double lon, double z, int w,
                          int h, int x0, int y0, int x1, int y1)
{
    file_ctx ctx;
    ctx.f = fopen(path, "rb");
    if (!ctx.f)
        return 1;
    fseeko(ctx.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(ctx.f);
    mm_vmap_io io = {file_read, &ctx, size};
    mm_vmap map;
    if (mm_vmap_open(&map, &io) != 0) {
        fclose(ctx.f);
        return 1;
    }
    size_t npix = (size_t)w * h;
    uint8_t *full = (uint8_t *)malloc(npix);
    uint8_t *strip = (uint8_t *)malloc(npix);
    void *scratchA = malloc(1 << 18);
    void *scratchB = malloc(1 << 18);
    if (!full || !strip || !scratchA || !scratchB) {
        fprintf(stderr, "oom\n");
        return 1;
    }
    chunk_cache ccA, ccB;
    memset(&ccA, 0, sizeof(ccA));
    memset(&ccB, 0, sizeof(ccB));
    ccA.map = &map;
    ccB.map = &map;
    mm_chunks srcA = {cache_get, cache_get_dir, &ccA};
    mm_chunks srcB = {cache_get, cache_get_dir, &ccB};
    mm_theme theme = {0, 255};
    mm_view view;
    view.z = z;
    view.cx = (lon + 180.0) / 360.0 * pow(2.0, z) * 256.0;
    view.cy = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * pow(2.0, z) * 256.0;
    view.width = w;
    view.height = h;

    mm_canvas cf = {full, w, h, w, {0, 0, w * MM_FP_ONE - 1, h * MM_FP_ONE - 1},
                    scratchA, 1 << 18};
    mm_canvas_clear(&cf, theme.bg);
    if (mm_render_view(&map, &srcA, &view, &cf, &theme) != 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    memset(strip, 0xAA, npix); /* sentinel: anything outside the clip is a bug */
    mm_canvas cs = {strip, w, h, w,
                    {x0 * MM_FP_ONE, y0 * MM_FP_ONE, x1 * MM_FP_ONE - 256,
                     y1 * MM_FP_ONE - 256},
                    scratchB, 1 << 18};
    /* the caller (composeScene) fills the clip with the background first */
    mm_canvas_fill_rect(&cs, cs.clip, theme.bg);
    if (mm_render_view(&map, &srcB, &view, &cs, &theme) != 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }
    int inside = 0, outside = 0, shown = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t v = strip[(size_t)y * w + x];
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                if (v != full[(size_t)y * w + x]) {
                    if (shown < 4) {
                        fprintf(stderr, "  diff at %d,%d full=%u strip=%u\n", x, y,
                                full[(size_t)y * w + x], v);
                        shown++;
                    }
                    inside++;
                }
            } else if (v != 0xAA) {
                outside++;
            }
        }
    }
    printf("strip %d,%d %dx%d: inside %d outside %d\n", x0, y0, x1 - x0, y1 - y0,
           inside, outside);
    cache_free(&ccA);
    cache_free(&ccB);
    free(full);
    free(strip);
    free(scratchA);
    free(scratchB);
    fclose(ctx.f);
    return 0;
}

/* ------------------------------------------------------------------ text --- */

/* Draws one label string with the shared atlas: the regression hook for the
 * UTF-8 decoder (accented letters, typographic punctuation). */
static int cmd_text(const char *text, int variant, const char *out, int w, int h)
{
    uint8_t *pix = (uint8_t *)malloc((size_t)w * h);
    void *scratch = malloc(1 << 16);
    if (!pix || !scratch) {
        fprintf(stderr, "oom\n");
        return 1;
    }
    mm_canvas canvas = {pix, w, h, w, {0, 0, w * MM_FP_ONE - 1, h * MM_FP_ONE - 1},
                        scratch, 1 << 16};
    mm_canvas_clear(&canvas, 255);
    if (mm_text_init() != 0) {
        fprintf(stderr, "text init failed\n");
        return 1;
    }
    int32_t tw = 0, top = 0, bot = 0;
    mm_text_metrics(text, variant, &tw, &top, &bot);
    mm_text_draw(&canvas, 4 * MM_FP_ONE, (h / 2) * MM_FP_ONE, text, variant, 0, 255);
    int rc = write_pgm(out, pix, w, h);
    fprintf(stderr, "text ok (variant %d, width %d)\n", variant,
            (int)(tw / MM_FP_ONE));
    free(pix);
    free(scratch);
    return rc;
}

/* ------------------------------------------------- reference renderer ---- */

/* Composes the same viewport with the old tile pipeline (render.c), which the
 * existing tests keep pixel identical to the Pillow renderer. */
static int cmd_refrender(const char *cache_path, double lat, double lon, double z,
                         int w, int h, const char *out, int dark, int no_labels)
{
    int zi = (int)floor(z + 0.5);
    if (zi < 0 || zi > 18) {
        fprintf(stderr, "zoom out of range\n");
        return 1;
    }
    mm_data d;
    if (mm_cache_read(cache_path, &d) != 0) {
        fprintf(stderr, "cannot read cache %s\n", cache_path);
        return 1;
    }
    if (no_labels)
        d.n_places = 0; /* golden pixel diffs run without labels */
    double *xy = mm_project_points(&d, zi);
    if (!xy) {
        fprintf(stderr, "projection failed\n");
        return 1;
    }
    mm_index *ix = mm_index_build(&d, zi, zi, xy);
    if (!ix) {
        fprintf(stderr, "index failed\n");
        return 1;
    }
    /* viewport centre in tile units at zi */
    double n = pow(2.0, zi);
    double ctx_ = (lon + 180.0) / 360.0 * n * 256.0;
    double cty = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * n * 256.0;
    uint8_t *pix = (uint8_t *)malloc((size_t)w * h);
    uint8_t *tile = (uint8_t *)malloc((size_t)256 * 256);
    if (!pix || !tile) {
        fprintf(stderr, "oom\n");
        return 1;
    }
    memset(pix, dark ? 0 : 255, (size_t)w * h);
    /* tiles covering the viewport */
    int tx0 = (int)floor((ctx_ - w / 2.0) / 256.0);
    int tx1 = (int)floor((ctx_ + w / 2.0) / 256.0);
    int ty0 = (int)floor((cty - h / 2.0) / 256.0);
    int ty1 = (int)floor((cty + h / 2.0) / 256.0);
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            mm_render_tile_img(ix, zi, tx, ty, dark, tile);
            for (int py = 0; py < 256; py++) {
                int dy = ty * 256 + py - (int)(cty - h / 2.0);
                if (dy < 0 || dy >= h)
                    continue;
                for (int px = 0; px < 256; px++) {
                    int dx = tx * 256 + px - (int)(ctx_ - w / 2.0);
                    if (dx < 0 || dx >= w)
                        continue;
                    pix[dy * w + dx] = tile[py * 256 + px];
                }
            }
        }
    }
    int rc = write_pgm(out, pix, w, h);
    free(pix);
    free(tile);
    mm_index_free(ix);
    free(xy);
    mm_data_free(&d);
    return rc;
}

/* ------------------------------------------------------------- fuzzing --- */

static void collect_dummy(void *user, uint32_t poi, int32_t score)
{
    (void)user;
    (void)poi;
    (void)score;
}

static int cmd_fuzzread(const char *path, int deep)
{
    file_ctx ctx;
    ctx.f = fopen(path, "rb");
    if (!ctx.f)
        return 1;
    fseeko(ctx.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(ctx.f);
    mm_vmap_io io = {file_read, &ctx, size};
    mm_vmap map;
    if (mm_vmap_open(&map, &io) != 0) {
        fclose(ctx.f);
        return 2; /* refused: the expected answer for corrupt input */
    }
    static uint8_t buf[1 << 20];
    for (int b = 0; b < map.n_bands && deep; b++) {
        const mm_vmap_band *bd = &map.bands[b];
        for (uint32_t cy = 0; cy < bd->ny; cy++) {
            for (uint32_t cx = 0; cx < bd->nx; cx++) {
                size_t len = 0;
                uint16_t flags = 0;
                if (mm_vmap_read_chunk(&map, b, bd->x0 + cx, bd->y0 + cy, buf,
                                       sizeof(buf), &len, &flags) != 0)
                    continue;
                mm_chunk_reader r;
                mm_chunk_reader_init(&r, buf, len);
                while (mm_chunk_next_record(&r) == 0) {
                    while (mm_chunk_next_point(&r) > 0) {
                    }
                }
                /* the record directory walks the same untrusted bytes: it must
                 * never read past the payload nor trip a sanitizer */
                mm_chunk_dir dir;
                if (mm_chunk_dir_build(&dir, buf, len, 1u << 20, dir_alloc, dir_free, NULL) == 0)
                    mm_chunk_dir_free(&dir, dir_free, NULL);
            }
        }
    }
    if (deep) {
        for (uint32_t i = 0; i < map.poi_count; i++) {
            mm_vmap_poi p;
            char name[128];
            if (mm_vmap_poi_get(&map, i, &p) == 0)
                mm_vmap_poi_name(&map, &p, name, sizeof(name));
        }
        if (map.has_poigrid) {
            uint32_t tx1 = map.pg_x0 + map.pg_nx - 1;
            uint32_t ty1 = map.pg_y0 + map.pg_ny - 1;
            mm_vmap_poi_foreach(&map, map.pg_x0, map.pg_y0, tx1, ty1, collect_dummy, NULL);
        }
    }
    fclose(ctx.f);
    return 0;
}

/* ---------------------------------------------------------------- bench -- */

static int cmd_bench(const char *path, double lat, double lon, double z, int w,
                     int h, int frames)
{
    file_ctx ctx;
    ctx.f = fopen(path, "rb");
    if (!ctx.f)
        return 1;
    fseeko(ctx.f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(ctx.f);
    mm_vmap_io io = {file_read, &ctx, size};
    mm_vmap map;
    if (mm_vmap_open(&map, &io) != 0) {
        fclose(ctx.f);
        return 1;
    }
    uint8_t *pix = (uint8_t *)malloc((size_t)w * h);
    void *scratch = malloc(1 << 18);
    chunk_cache cc;
    memset(&cc, 0, sizeof(cc));
    cc.map = &map;
    mm_chunks src = {cache_get, cache_get_dir, &cc};
    mm_canvas canvas = {pix, w, h, w, {0, 0, w * MM_FP_ONE - 1, h * MM_FP_ONE - 1},
                        scratch, 1 << 18};
    mm_theme theme = {0, 255};
    mm_view view;
    view.z = z;
    view.cx = (lon + 180.0) / 360.0 * pow(2.0, z) * 256.0;
    view.cy = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * pow(2.0, z) * 256.0;
    view.width = w;
    view.height = h;

    double t0 = mm_now();
    for (int i = 0; i < frames; i++) {
        /* pan a little each frame like the device does */
        view.cx += 2.0;
        if (mm_render_view(&map, &src, &view, &canvas, &theme) != 0) {
            fprintf(stderr, "render failed\n");
            return 1;
        }
    }
    double dt = mm_now() - t0;
    printf("bench: %d frames %dx%d z%.2f  avg %.2f ms/frame  reads %llu hits %llu "
           "decompressed %.2f MB\n",
           frames, w, h, z, dt * 1000.0 / frames, (unsigned long long)cc.misses,
           (unsigned long long)cc.hits, (double)cc.bytes / 1048576.0);
    cache_free(&cc);
    free(pix);
    free(scratch);
    fclose(ctx.f);
    return 0;
}

/* ------------------------------------------------------------------ main - */

static void usage(void)
{
    fprintf(stderr,
            "mm_host render    --input B.mmap --lat X --lon X --zoom Z --width W "
            "--height H --out X.pgm [--dark] [--no-labels] [--no-dirs]\n"
            "mm_host refrender --cache C.mosmb --lat X --lon X --zoom Z --width W "
            "--height H --out X.pgm [--dark]\n"
            "mm_host fuzzread  --input B.mmap [--deep]\n"
            "mm_host bench     --input B.mmap --lat X --lon X --zoom Z --width W "
            "--height H --frames N\n"
            "mm_host cachetest --input B.mmap --lat X --lon X --zoom Z --width W "
            "--height H [--pan-x N --pan-y N] [--out A.pgm --out2 B.pgm] [--dark] "
            "[--block-rebuild]\n"
            "mm_host text      --text STR --out X.pgm [--variant N] "
            "[--width W --height H]\n"
            "mm_host stripcheck --input B.mmap --lat X --lon X --zoom Z --width W "
            "--height H --x0 A --y0 B --x1 C --y1 D\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    const char *input = NULL, *cache = NULL, *out = NULL, *out2 = NULL;
    const char *text = NULL;
    double lat = 0, lon = 0, z = 12;
    double panx = 0, pany = 0;
    int w = 480, h = 583, dark = 0, deep = 0, frames = 20, no_labels = 0, no_dirs = 0;
    int block_rebuild = 0, variant = 0;
    int x0 = 0, y0 = 0, x1 = 480, y1 = 583;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--input") && i + 1 < argc)
            input = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            cache = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--out2") && i + 1 < argc)
            out2 = argv[++i];
        else if (!strcmp(argv[i], "--pan-x") && i + 1 < argc)
            panx = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pan-y") && i + 1 < argc)
            pany = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lat") && i + 1 < argc)
            lat = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lon") && i + 1 < argc)
            lon = atof(argv[++i]);
        else if (!strcmp(argv[i], "--zoom") && i + 1 < argc)
            z = atof(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dark"))
            dark = 1;
        else if (!strcmp(argv[i], "--deep"))
            deep = 1;
        else if (!strcmp(argv[i], "--no-labels"))
            no_labels = 1;
        else if (!strcmp(argv[i], "--no-dirs"))
            no_dirs = 1;
        else if (!strcmp(argv[i], "--block-rebuild"))
            block_rebuild = 1;
        else if (!strcmp(argv[i], "--text") && i + 1 < argc)
            text = argv[++i];
        else if (!strcmp(argv[i], "--variant") && i + 1 < argc)
            variant = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--x0") && i + 1 < argc)
            x0 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--y0") && i + 1 < argc)
            y0 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--x1") && i + 1 < argc)
            x1 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--y1") && i + 1 < argc)
            y1 = atoi(argv[++i]);
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!strcmp(cmd, "render") && input && out)
        return cmd_render(input, lat, lon, z, w, h, out, dark, no_labels, no_dirs);
    if (!strcmp(cmd, "refrender") && cache && out)
        return cmd_refrender(cache, lat, lon, z, w, h, out, dark, no_labels);
    if (!strcmp(cmd, "fuzzread") && input)
        return cmd_fuzzread(input, deep);
    if (!strcmp(cmd, "bench") && input)
        return cmd_bench(input, lat, lon, z, w, h, frames);
    if (!strcmp(cmd, "cachetest") && input)
        return cmd_cachetest(input, lat, lon, z, w, h, panx, pany, out, out2, dark,
                             block_rebuild);
    if (!strcmp(cmd, "text") && text && out)
        return cmd_text(text, variant, out, w, h);
    if (!strcmp(cmd, "stripcheck") && input)
        return cmd_stripcheck(input, lat, lon, z, w, h, x0, y0, x1, y1);
    usage();
    return 2;
}
