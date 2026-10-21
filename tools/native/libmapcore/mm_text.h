/* Text rendering on top of the glyph atlas (tools/native/glyph_atlas.c).
 *
 * Two jobs:
 *  - UTF-8 decoding: the atlas is indexed by Unicode codepoint 32..255 (Latin-1
 *    range, which covers every Italian toponym accent) but the old pipeline
 *    indexed it per *byte*, so "Citta'" with an accent came out as mojibake.
 *    Here multi-byte UTF-8 is decoded properly, codepoints outside the atlas
 *    fall back to '?', and the common typographic punctuation (U+2018/2019
 *    curly apostrophes, dashes) is normalised to ASCII so real OSM names
 *    never show a question mark.
 *  - runtime halo: the mask is dilated by blitting itself at the offsets with
 *    dx^2+dy^2 <= 5 (exactly the reference raster pipeline's stroke), no
 *    pre-baked halos in the atlas, so the 1-bit and 4-level passes of the
 *    e-ink refresh draw the same thing.
 */
#ifndef MM_TEXT_H
#define MM_TEXT_H

#include "mm_core.h"

/* Builds the glyph index from the atlas blob (idempotent). */
int mm_text_init(void);

/* Advance width and vertical ink extent of a string in 24.8 px. */
void mm_text_metrics(const char *text, int variant, int32_t *width24,
                     int32_t *top24, int32_t *bottom24);

/* Draws one text run with halo: halo pass paints bg, the glyph pass blends
 * the pre-rendered coverage on top. */
void mm_text_draw(mm_canvas *c, int32_t x24, int32_t y24, const char *text,
                  int variant, uint8_t fg, uint8_t bg);

#endif /* MM_TEXT_H */
