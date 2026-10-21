#include "mm_text.h"
#include "mm_raster.h"
#include "glyph_atlas.h"

#include <stdlib.h>
#include <string.h>

#define ATLAS_CODES (MONOMESH_ATLAS_LAST_CHAR - MONOMESH_ATLAS_FIRST_CHAR + 1)

typedef struct {
    uint32_t offset;
    uint16_t len;
} mm_glyph;

static mm_glyph *g_index;
static const unsigned char *g_data;
static int g_ready;

int mm_text_init(void)
{
    if (g_ready)
        return 0;
    g_index = (mm_glyph *)malloc(sizeof(mm_glyph) * (size_t)ATLAS_CODES *
                                 MONOMESH_ATLAS_VARIANTS);
    if (!g_index)
        return -1;
    g_data = monomesh_glyph_atlas;
    uint32_t pos = 0;
    for (int v = 0; v < MONOMESH_ATLAS_VARIANTS; v++) {
        for (int c = 0; c < ATLAS_CODES; c++) {
            mm_glyph *g = &g_index[v * ATLAS_CODES + c];
            if (pos + 6 > monomesh_glyph_atlas_len)
                return -1;
            unsigned w = g_data[pos], h = g_data[pos + 1];
            uint32_t plen = 6u + (uint32_t)w * h;
            g->offset = pos;
            g->len = (uint16_t)plen;
            pos += plen;
        }
    }
    g_ready = 1;
    return 0;
}

/* Decodes one UTF-8 codepoint and normalises typographic punctuation.
 * Returns bytes consumed (>=1); *cp is the codepoint or '?'.
 *
 * The sequence length is decided on the LEAD byte: checking the decoded
 * codepoint against 0xf0 (the old code) swallowed every Latin-1 letter from
 * 0xF0 up - ò, ó, ù, ú, ü, ñ... - as a bogus 4-byte sequence and turned them
 * into '?' ("Cantù" -> "Cant?"). */
static int utf8_next(const unsigned char *s, uint32_t *cp)
{
    const uint32_t lead = s[0];
    uint32_t c = lead;
    int n = 1;
    if (lead >= 0xf0) {
        /* 4-byte sequence: outside the atlas (emoji & co), consume it whole so
         * the continuation bytes cannot turn into bogus glyphs */
        while (n < 4 && s[n])
            n++;
        c = '?';
    } else if (lead >= 0xe0) {
        if (s[1] && s[2]) {           /* 3-byte: typographic punctuation */
            c = ((lead & 0x0fu) << 12) | ((s[1] & 0x3fu) << 6) | (s[2] & 0x3fu);
            n = 3;
        }
    } else if (lead >= 0xc0) {
        if (s[1]) {                   /* 2-byte: Latin-1 letters */
            c = ((lead & 0x1fu) << 6) | (s[1] & 0x3fu);
            n = 2;
        }
    }
    /* normalise typographic punctuation to ASCII/Latin-1 */
    switch (c) {
    case 0x2018: case 0x2019: case 0x201b: case 0x00b4:
        c = '\'';
        break;
    case 0x201c: case 0x201d:
        c = '"';
        break;
    case 0x2013: case 0x2014: case 0x2212:
        c = '-';
        break;
    case 0x2026:
        c = '.';
        break;
    default:
        break;
    }
    *cp = c;
    return n;
}

static const mm_glyph *glyph_for(int variant, uint32_t cp)
{
    if (cp < MONOMESH_ATLAS_FIRST_CHAR || cp > MONOMESH_ATLAS_LAST_CHAR)
        cp = '?';
    return &g_index[variant * ATLAS_CODES + (int)(cp - MONOMESH_ATLAS_FIRST_CHAR)];
}

void mm_text_metrics(const char *text, int variant, int32_t *width24,
                     int32_t *top24, int32_t *bottom24)
{
    int32_t w = 0, top = 0, bottom = 0;
    int first = 1;
    if (!text || mm_text_init() != 0) {
        *width24 = *top24 = *bottom24 = 0;
        return;
    }
    for (const unsigned char *s = (const unsigned char *)text; *s;) {
        uint32_t cp;
        s += utf8_next(s, &cp);
        const mm_glyph *g = glyph_for(variant, cp);
        const unsigned char *gl = g_data + g->offset;
        int gh = gl[1];
        int gtop = (int8_t)gl[3];
        int advance = (int16_t)(gl[4] | (gl[5] << 8));
        if (gh > 0) {
            if (first || gtop < top)
                top = gtop;
            if (first || gtop + gh > bottom)
                bottom = gtop + gh;
            first = 0;
        }
        w += advance;
    }
    *width24 = ((w + 32) >> 6) * MM_FP_ONE;
    *top24 = top * MM_FP_ONE;
    *bottom24 = bottom * MM_FP_ONE;
}

/* Halo offsets: the dilation set of the reference pipeline (dx^2+dy^2 <= 5). */
static const int8_t HALO_OFF[][2] = {
    {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    {2, 0}, {-2, 0}, {0, 2}, {0, -2}, {2, 1}, {2, -1}, {-2, 1}, {-2, -1},
    {1, 2}, {1, -2}, {-1, 2}, {-1, -2},
};
#define HALO_COUNT ((int)(sizeof(HALO_OFF) / sizeof(HALO_OFF[0])))

void mm_text_draw(mm_canvas *c, int32_t x24, int32_t y24, const char *text,
                  int variant, uint8_t fg, uint8_t bg)
{
    if (!text || mm_text_init() != 0)
        return;

    for (int pass = 0; pass < 2; pass++) {
        int64_t pen = (int64_t)mm_px(x24) << 6; /* 26.6, like the atlas */
        for (const unsigned char *s = (const unsigned char *)text; *s;) {
            uint32_t cp;
            s += utf8_next(s, &cp);
            const mm_glyph *g = glyph_for(variant, cp);
            const unsigned char *gl = g_data + g->offset;
            int w = gl[0], h = gl[1];
            int left = (int8_t)gl[2], top = (int8_t)gl[3];
            int advance = (int16_t)(gl[4] | (gl[5] << 8));
            const unsigned char *cov = gl + 6;
            int gx = (int)((pen + 32) >> 6) + left, gy = mm_px(y24) + top;

            if (pass == 0) {
                /* halo: dilate the binary mask by blitting it at each offset */
                for (int o = 0; o < HALO_COUNT; o++) {
                    int dx = HALO_OFF[o][0], dy = HALO_OFF[o][1];
                    for (int yy = 0; yy < h; yy++) {
                        for (int xx = 0; xx < w; xx++) {
                            if (cov[yy * w + xx] < 128)
                                continue;
                            mm_canvas_put(c, gx + xx + dx, gy + yy + dy, bg);
                        }
                    }
                }
            } else {
                for (int yy = 0; yy < h; yy++) {
                    for (int xx = 0; xx < w; xx++) {
                        int cv = cov[yy * w + xx];
                        if (!cv)
                            continue;
                        int px = gx + xx, py = gy + yy;
                        if (px < 0 || py < 0 || px >= c->width || py >= c->height)
                            continue;
                        if (px < mm_px(c->clip.x0) || px > mm_px(c->clip.x1) ||
                            py < mm_px(c->clip.y0) || py > mm_px(c->clip.y1))
                            continue;
                        uint8_t *dst = &c->pixels[(int32_t)py * c->stride + px];
                        if (cv >= 255) {
                            *dst = fg;
                        } else {
                            int old = *dst;
                            *dst = (uint8_t)((fg * cv + old * (255 - cv)) / 255);
                        }
                    }
                }
            }
            pen += advance;
        }
    }
}
