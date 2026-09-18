#!/usr/bin/env python3
"""Rasterize the big clock digits into a C header (1-bit, native resolution).

The e-paper clock must look crisp: scaling a 24 pt bitmap font up (the first
implementation did that) multiplies the glyph pixels, and the blockiness is
obvious on a 480x800 panel. This tool renders the digits directly at the target
pixel height from a TTF, so the firmware embeds glyphs that are already at the
final resolution.

Output format: for each glyph a 1-bit bitmap, one row per line, MSB first, each
row padded to a byte (the classic Adafruit_GFX layout), plus the metrics needed
to place the ink on a baseline (yOffset negative = above the baseline). The
blitter in src/clock_mode.cpp walks the runs of black pixels of every row and
draws them with fillRect().

Usage:
    python3 tools/gen_clock_font.py --height 225 --name ClockFontLarge \
        --out src/clock_font_large.h
    python3 tools/gen_clock_font.py --height 142 --name ClockFontSmall \
        --out src/clock_font_small.h

Default font: Roboto Condensed Bold (Apache-2.0, see THIRD_PARTY_NOTICES.md).
Any TTF with tabular digits works; --list prints the built-in candidates.
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is required: python3 -m pip install -r requirements.txt")

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/roboto/unhinted/RobotoCondensed-Bold.ttf",
    "/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/Roboto-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
]

GLYPHS = "0123456789:"
SUPERSAMPLE = 4          # render at 4x and downsample: clean 1-bit edges
THRESHOLD = 128          # 0..255 -> black below


def pick_font(explicit):
    if explicit:
        if not os.path.exists(explicit):
            sys.exit(f"font not found: {explicit}")
        return explicit
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return p
    sys.exit("no usable TTF found, pass --font <path>")


def render_mask(font, ch, ss=1):
    """Render one glyph and return (pixels, pen_x, pen_y, ascent).

    The pen (left end of the baseline) sits at (margin, margin + ascent); the
    returned image is the whole canvas at 1/ss of the rendering scale, so image
    pixel coordinates are final-size coordinates with that pen as origin.
    """
    ascent, descent = font.getmetrics()
    m = 4 * ss
    w = int(font.getlength(ch)) + ascent + 2 * m
    h = ascent + descent + 2 * m
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    ox, oy = m, m + ascent  # ls = left, baseline: the pen is the baseline start
    d.text((ox, oy), ch, font=font, fill=0, anchor="ls")
    if ss > 1:
        img = img.resize((w // ss, h // ss), Image.LANCZOS)
        ox, oy = ox // ss, oy // ss
    return img, ox, oy, ascent / ss


def ink_box(img, thresh=THRESHOLD):
    """Bounding box of the dark pixels; (x0, y0, x1, y1), inclusive, or None."""
    mask = img.point(lambda v: 255 if v < thresh else 0)
    box = mask.getbbox()  # right/lower are exclusive
    if box is None:
        return None
    return box[0], box[1], box[2] - 1, box[3] - 1


def render_glyph(font_path, em, ch):
    """Return (rows, width, height, x_offset, y_offset) at the final pixel size.

    x_offset/y_offset are relative to the pen position on the baseline (yOffset
    negative = above the baseline), like the Adafruit/M5GFX font convention.
    """
    ss = SUPERSAMPLE
    big = ImageFont.truetype(font_path, em * ss)
    img, pen_x, pen_y, ascent = render_mask(big, ch, ss=1)
    box = ink_box(img, thresh=200)  # any anti-aliased ink, to size the crop
    if box is None:
        return None
    x0, y0, x1, y1 = box
    # Crop with one final-size pixel of margin so the downsampling keeps the
    # anti-aliasing around the edges, then threshold and crop tight.
    m = ss
    crop = img.crop((max(0, x0 - m), max(0, y0 - m), x1 + 1 + m, y1 + 1 + m))
    small = crop.resize(
        ((crop.width + ss - 1) // ss, (crop.height + ss - 1) // ss), Image.LANCZOS)
    box2 = ink_box(small)
    if box2 is None:
        return None
    sx0, sy0, sx1, sy1 = box2
    gw = sx1 - sx0 + 1
    gh = sy1 - sy0 + 1
    px = small.load()
    rows = []
    for y in range(sy0, sy1 + 1):
        row = bytearray((gw + 7) // 8)
        for x in range(sx0, sx1 + 1):
            if px[x, y] < THRESHOLD:
                row[(x - sx0) >> 3] |= 0x80 >> ((x - sx0) & 7)
        rows.append(bytes(row))
    # Map the tight box back to the pen, all in final pixels: the crop origin
    # is a big-size coordinate, the margin and the pen are converted too.
    x_offset = (max(0, x0 - m) + sx0 * ss - pen_x) / ss
    y_offset = (max(0, y0 - m) + sy0 * ss - pen_y) / ss
    return rows, gw, gh, int(round(x_offset)), int(round(y_offset))


def digit_height_of(font_path, em, ch="0"):
    ft = ImageFont.truetype(font_path, em)
    img, *_ = render_mask(ft, ch)
    box = ink_box(img)
    return 0 if box is None else box[3] - box[1] + 1


def em_for_height(font_path, target_h):
    """Find the pixel size whose '0' ink is exactly target_h px tall."""
    ref = 400
    h = digit_height_of(font_path, ref)
    if h <= 0:
        sys.exit("the font has no usable digit glyphs")
    em = max(8, int(round(ref * target_h / h)))
    h2 = digit_height_of(font_path, em)
    if h2 > 0 and h2 != target_h:
        em = max(8, int(round(em * target_h / h2)))
    return em


def emit_glyph_const(name, rows):
    out = [f"static const uint8_t {name}[] PROGMEM = {{"]
    for r in rows:
        out.append("    " + " ".join(f"0x{b:02X}," for b in r))
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="TTF path (default: the first existing candidate)")
    ap.add_argument("--height", type=int, required=True, help="digit ink height in pixels")
    ap.add_argument("--name", required=True, help="C identifier prefix (e.g. ClockFontLarge)")
    ap.add_argument("--out", required=True, help="output header path")
    ap.add_argument("--preview", help="also write a PNG preview of the digits")
    ap.add_argument("--list", action="store_true", help="list the font candidates and exit")
    args = ap.parse_args()

    if args.list:
        for p in FONT_CANDIDATES:
            print(("[x] " if os.path.exists(p) else "[ ] ") + p)
        return

    font_path = pick_font(args.font)
    em = em_for_height(font_path, args.height)
    ft = ImageFont.truetype(font_path, em)

    glyphs = {}
    for ch in GLYPHS:
        g = render_glyph(font_path, em, ch)
        if g is None:
            sys.exit(f"could not rasterize {ch!r}")
        glyphs[ch] = g

    digit_advance = int(round(ft.getlength("0")))
    colon_advance = int(round(ft.getlength(":")))
    digit_h = glyphs["0"][2]
    total_w = 4 * digit_advance + colon_advance
    print(f"em={em}px digit={glyphs['0'][1]}x{digit_h} advance={digit_advance} "
          f"colon_adv={colon_advance} total_width={total_w}")
    for ch in GLYPHS:
        rows, w, h, xo, yo = glyphs[ch]
        if xo < 0 or xo + w > digit_advance + 1:
            print(f"warning: {ch!r} ink [{xo}..{xo+w}] does not fit advance {digit_advance}")

    lines = [
        f"// Generated by tools/gen_clock_font.py from {os.path.basename(font_path)}"
        f" at {args.height} px digit height.",
        "// Do not edit by hand: rerun the tool (see its header comment).",
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "#include \"clock_mode.h\"",
        "",
        "namespace MonoMesh {",
        f"namespace {args.name} {{",
        "",
        f"static constexpr int DIGIT_HEIGHT = {digit_h};",
        f"static constexpr int DIGIT_ADVANCE = {digit_advance};",
        f"static constexpr int COLON_ADVANCE = {colon_advance};",
        "",
    ]

    for ch in "0123456789":
        rows, w, h, xo, yo = glyphs[ch]
        lines.append(emit_glyph_const(f"kGlyph{ch}", rows))
        lines.append("")

    rows, w, h, xo, yo = glyphs[":"]
    lines.append(emit_glyph_const("kGlyphColon", rows))
    lines.append("")

    lines.append("static const ClockGlyph kDigits[10] PROGMEM = {")
    for ch in "0123456789":
        rows, w, h, xo, yo = glyphs[ch]
        lines.append(f"    {{ kGlyph{ch}, {w}, {h}, {xo}, {yo} }},")
    lines.append("};")
    rows, w, h, xo, yo = glyphs[":"]
    lines.append(f"static const ClockGlyph kColon PROGMEM = {{ kGlyphColon, {w}, {h}, {xo}, {yo} }};")
    lines.append("")
    lines.append(f"}} // namespace {args.name}")
    lines.append("} // namespace MonoMesh")
    lines.append("")

    with open(args.out, "w") as f:
        f.write("\n".join(lines))
    total = sum(len(rows[0]) * len(rows) for rows, *_ in glyphs.values())
    print(f"{args.out}: flash≈{total} B")

    if args.preview:
        # Draw "88:88" with the generated data, exactly like the firmware blitter does.
        pad = 20
        adv = digit_advance if digit_advance % 2 == 0 else digit_advance + 1
        img = Image.new("1", (4 * adv + colon_advance + 2 * pad, digit_h + 2 * pad), 1)
        d = ImageDraw.Draw(img)
        baseline = pad + digit_h
        seq = [("8", pad), ("8", pad + adv), (":", pad + 2 * adv),
               ("8", pad + 2 * adv + colon_advance), ("8", pad + 3 * adv + colon_advance)]
        for ch, pen in seq:
            rows, w, h, xo, yo = glyphs[ch]
            for y, row in enumerate(rows):
                for x in range(w):
                    if row[x >> 3] & (0x80 >> (x & 7)):
                        px = pen + xo + x
                        py = baseline + yo + y
                        if 0 <= px < img.width and 0 <= py < img.height:
                            d.point((px, py), 0)
        img.save(args.preview)
        print(f"preview written to {args.preview}")


if __name__ == "__main__":
    main()
