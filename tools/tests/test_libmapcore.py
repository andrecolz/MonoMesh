#!/usr/bin/env python3
"""Host tests for libmapcore (Piano B: vector container + on-device rasteriser).

Strategy (plan Fase 1): everything that can be checked on the host is checked
here, so the device only needs the collaudo of what a PC cannot see.

* golden scenes: the new fixed point core vs the old tile pipeline (which the
  existing suite keeps pixel identical to Pillow).  The cores do not share
  code, and the vector path simplifies geometry on purpose, so the metric is
  spatial: ink mass within 5%, and at most 2% of the ink further than 1 px
  from the other renderer's ink.  Labels are excluded from the pixel metric
  (per screen vs per tile layout) and checked structurally instead.
* determinism: same view twice => bit identical output, including fractional
  zoom (the fixed point path must not depend on rounding history).
* fuzzing: mutated/truncated containers through the UBSan reader (the
  bundled Zig toolchain ships no ASan runtime) - the reader must refuse them
  or decode them, never crash, never trip a sanitizer.
* v1 compatibility: the v2 reader opens the v1 raster container (basemap).
* Python cross check: a from-scratch Python parser agrees with the C tool.

The tests are skipped when the native tools cannot be built.
"""

import glob
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_pipeline import fot, make_synthetic_pbf  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
NATIVE = os.path.join(TOOLS, "native")
REPO = os.path.dirname(TOOLS)

VMAP = os.path.join(NATIVE, "monomesh-vmap")
MM_HOST = os.path.join(NATIVE, "mm_host")
MM_HOST_UBSAN = os.path.join(NATIVE, "mm_host_ubsan")
BUILD_VMAP = os.path.join(NATIVE, "build_vmap.sh")
BUILD_HOST = os.path.join(NATIVE, "build_mm_host.sh")
TILES = os.path.join(NATIVE, "monomesh-tiles")
BUILD_TILES = os.path.join(NATIVE, "build.sh")


def build_tool(binary, script, extra=()):
    if os.path.exists(binary):
        return True
    if not os.path.exists(script):
        return False
    proc = subprocess.run(["sh", script, *extra], capture_output=True, text=True)
    return proc.returncode == 0 and os.path.exists(binary)


def have_tools():
    return (build_tool(VMAP, BUILD_VMAP) and build_tool(MM_HOST, BUILD_HOST) and
            build_tool(TILES, BUILD_TILES))


def have_ubsan():
    return build_tool(MM_HOST_UBSAN, BUILD_HOST, extra=("--ubsan",))


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P5\n<w> <h>\n255\n<raw>
    parts = data.split(b"\n", 3)
    if parts[0] != b"P5":
        raise ValueError("not a PGM: %s" % path)
    w, h = (int(v) for v in parts[1].split())
    return Image.frombytes("L", (w, h), parts[3][: w * h])


def ink_mask(img):
    return img.point(lambda p: 255 if p < 128 else 0)


def ink_count(mask):
    return sum(1 for p in mask.getdata() if p)


def spatial_mismatch(a, b, radius=1):
    """Ink of a farther than `radius` px from any ink of b (and vice versa)."""
    da = a.filter(ImageFilter.MaxFilter(2 * radius + 1))
    db = b.filter(ImageFilter.MaxFilter(2 * radius + 1))
    pa, pb, qa, qb = a.load(), b.load(), da.load(), db.load()
    w, h = a.size
    m = e = 0
    for y in range(h):
        for x in range(w):
            if pa[x, y] and not qb[x, y]:
                m += 1
            if pb[x, y] and not qa[x, y]:
                e += 1
    return m, e


@unittest.skipUnless(have_tools(), "native tools cannot be built")
class VmapFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="monomesh-libmapcore-")
        cls.pbf = os.path.join(cls.tmp, "synthetic.osm.pbf")
        cls.info = make_synthetic_pbf(cls.pbf, span=0.30)
        cls.bbox = cls.info["bbox"]
        cls.cache_dir = os.path.join(cls.tmp, "cache")
        cmd = [TILES, "--pbf", cls.pbf, "--bbox", *[str(v) for v in cls.bbox],
               "--zooms", "13,15", "--output", os.path.join(cls.tmp, "tiles"),
               "--cache-dir", cls.cache_dir, "--jobs", "2", "--quiet"]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError("cache build failed: %s" % proc.stderr[-2000:])
        caches = glob.glob(os.path.join(cls.cache_dir, "*.mosmb"))
        if not caches:
            raise RuntimeError("no .mosmb cache produced")
        cls.cache = caches[0]
        cls.vmap = os.path.join(cls.tmp, "map-v2.mmap")
        proc = subprocess.run([VMAP, "generate", "--cache", cls.cache,
                               "--output", cls.vmap, "--name", "synthetic",
                               "--quiet"],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError("generate failed: %s" % proc.stderr[-2000:])

    def render(self, lat, lon, z, out, labels=False, dark=False, tool=MM_HOST,
               input_path=None, cache=None):
        cmd = [tool, "render" if input_path else "refrender",
               "--input" if input_path else "--cache",
               input_path or cache or self.cache,
               "--lat", str(lat), "--lon", str(lon), "--zoom", str(z),
               "--width", "480", "--height", "583", "--out", out]
        if not labels:
            cmd.append("--no-labels")
        if dark:
            cmd.append("--dark")
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        return read_pgm(out)


# --------------------------------------------------------------------------- #
# Golden scenes vs the reference renderer
# --------------------------------------------------------------------------- #

GOLDEN_SCENES = [
    # name, lat, lon, zoom (synthetic fixture centre)
    ("grid-z13", 45.85, 9.15, 13),
    ("grid-z14", 45.85, 9.15, 14),
    ("grid-z15", 45.85, 9.15, 15),
    ("grid-z12-off", 45.78, 9.06, 12),
]


@unittest.skipUnless(have_tools(), "native tools cannot be built")
class TestGoldenAgainstReference(VmapFixture):
    def test_map_layers_match_reference(self):
        for name, lat, lon, z in GOLDEN_SCENES:
            with self.subTest(scene=name):
                a = ink_mask(self.render(lat, lon, z,
                                         os.path.join(self.tmp, name + "-ref.pgm"),
                                         tool=MM_HOST, input_path=None))
                b = ink_mask(self.render(lat, lon, z,
                                         os.path.join(self.tmp, name + "-new.pgm"),
                                         tool=MM_HOST, input_path=self.vmap))
                ia, ib = ink_count(a), ink_count(b)
                self.assertGreater(ia, 100, "scene too empty for a golden test")
                # ink mass within 5%
                self.assertLess(abs(ia - ib) / ia, 0.05,
                                "%s: ink %d vs %d" % (name, ia, ib))
                # at most 2% of the union is farther than 1 px from the other
                missing, extra = spatial_mismatch(a, b, radius=1)
                union = ia + ib - ink_count(Image.frombytes(
                    "L", a.size, bytes(min(x, y) for x, y in zip(a.getdata(),
                                                                b.getdata()))))
                self.assertLess(missing / union, 0.02,
                                "%s: %d ref px unmatched" % (name, missing))
                self.assertLess(extra / union, 0.02,
                                "%s: %d new px unmatched" % (name, extra))

    def test_dark_theme_inverts(self):
        light = self.render(45.85, 9.15, 13,
                            os.path.join(self.tmp, "light.pgm"),
                            input_path=self.vmap)
        dark = self.render(45.85, 9.15, 13,
                           os.path.join(self.tmp, "dark.pgm"),
                           input_path=self.vmap, dark=True)
        il = ink_count(ink_mask(light))
        idark = sum(1 for p in dark.getdata() if p > 128)
        self.assertGreater(il, 100)
        self.assertLess(abs(il - idark) / il, 0.05)


# --------------------------------------------------------------------------- #
# Determinism (property test)
# --------------------------------------------------------------------------- #

@unittest.skipUnless(have_tools(), "native tools cannot be built")
class TestDeterminism(VmapFixture):
    def test_same_view_twice_bit_identical(self):
        for i, (lat, lon, z) in enumerate([(45.85, 9.15, 13), (45.85, 9.15, 15.5),
                                           (45.80, 9.05, 11.25)]):
            out1 = os.path.join(self.tmp, "det%d-a.pgm" % i)
            out2 = os.path.join(self.tmp, "det%d-b.pgm" % i)
            self.render(lat, lon, z, out1, labels=True, input_path=self.vmap)
            self.render(lat, lon, z, out2, labels=True, input_path=self.vmap)
            with open(out1, "rb") as f1, open(out2, "rb") as f2:
                self.assertEqual(f1.read(), f2.read(),
                                 "fractional zoom %s must be deterministic" % z)

    def test_random_viewports_render(self):
        rng = random.Random(1234)
        for i in range(6):
            lat = 45.70 + rng.random() * 0.30
            lon = 9.00 + rng.random() * 0.30
            z = 7 + rng.random() * 10  # fractional, across all bands
            out = os.path.join(self.tmp, "rnd%d.pgm" % i)
            self.render(lat, lon, z, out, labels=True, input_path=self.vmap)

    def test_record_directory_cull_is_bit_exact(self):
        """The per-record bbox cull must never change a pixel: the same scene
        rendered through the directory path and through the legacy walk must be
        byte identical (plan: strips skip whole records before decoding)."""
        for i, (lat, lon, z) in enumerate([(45.85, 9.15, 12), (45.85, 9.15, 13.5),
                                           (45.80, 9.05, 15)]):
            with_dirs = os.path.join(self.tmp, "dirs%d.pgm" % i)
            legacy = os.path.join(self.tmp, "legacy%d.pgm" % i)
            base = [MM_HOST, "render", "--input", self.vmap, "--lat", str(lat),
                    "--lon", str(lon), "--zoom", str(z), "--width", "480",
                    "--height", "583"]
            p1 = subprocess.run(base + ["--out", with_dirs],
                                capture_output=True, text=True)
            p2 = subprocess.run(base + ["--out", legacy, "--no-dirs"],
                                capture_output=True, text=True)
            self.assertEqual(p1.returncode, 0, p1.stderr[-2000:])
            self.assertEqual(p2.returncode, 0, p2.stderr[-2000:])
            with open(with_dirs, "rb") as f1, open(legacy, "rb") as f2:
                self.assertEqual(f1.read(), f2.read(),
                                 "directory cull changed pixels at z%s" % z)


# --------------------------------------------------------------------------- #
# Structure, verify, v1 compatibility
# --------------------------------------------------------------------------- #

@unittest.skipUnless(have_tools(), "native tools cannot be built")
class TestContainerStructure(VmapFixture):
    def test_verify_deep(self):
        proc = subprocess.run([VMAP, "verify", "--input", self.vmap, "--deep"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("[OK]", proc.stdout)

    def test_info_reports_bands(self):
        proc = subprocess.run([VMAP, "info", "--input", self.vmap],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0)
        self.assertIn("band 0", proc.stdout)
        self.assertIn("band 2", proc.stdout)
        self.assertIn("version    : 2", proc.stdout)

    def test_style_version_is_checked(self):
        raw = bytearray(open(self.vmap, "rb").read(16))
        flags = struct.unpack_from("<I", raw, 12)[0]
        struct.pack_into("<I", raw, 12, (flags & 0xFFFF) | (99 << 16))
        broken = os.path.join(self.tmp, "bad-style.mmap")
        with open(broken, "wb") as f:
            f.write(raw)
            f.write(open(self.vmap, "rb").read())
        proc = subprocess.run([MM_HOST, "fuzzread", "--input", broken],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2, "style mismatch must be refused")

    def test_v1_raster_container_still_opens(self):
        basemap = os.path.join(REPO, "maps", "basemap.mmap")
        if not os.path.exists(basemap):
            self.skipTest("basemap.mmap not present")
        proc = subprocess.run([MM_HOST, "fuzzread", "--input", basemap],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0,
                         "the v2 reader must open the v1 raster basemap: %s"
                         % proc.stderr[-2000:])


# --------------------------------------------------------------------------- #
# Labels (structural checks; the pixel metric excludes them on purpose)
# --------------------------------------------------------------------------- #

@unittest.skipUnless(have_tools(), "native tools cannot be built")
class TestLabels(VmapFixture):
    def test_expected_labels_are_present(self):
        # the synthetic fixture has "Citta Grande" (city) at 45.7633, 9.04
        img = self.render(45.7633, 9.04, 12, os.path.join(self.tmp, "labels.pgm"),
                          labels=True, input_path=self.vmap)
        no_labels = self.render(45.7633, 9.04, 12,
                                os.path.join(self.tmp, "nolabels.pgm"),
                                labels=False, input_path=self.vmap)
        with_l = ink_mask(img)
        without = ink_mask(no_labels)
        extra = sum(1 for a, b in zip(with_l.getdata(), without.getdata())
                    if a and not b)
        self.assertGreater(extra, 40, "expected label ink near the city POI")

    def test_label_layout_identical_between_runs(self):
        out1 = os.path.join(self.tmp, "lab1.pgm")
        out2 = os.path.join(self.tmp, "lab2.pgm")
        self.render(45.7633, 9.04, 13, out1, labels=True, input_path=self.vmap)
        self.render(45.7633, 9.04, 13, out2, labels=True, input_path=self.vmap)
        with open(out1, "rb") as f1, open(out2, "rb") as f2:
            self.assertEqual(f1.read(), f2.read())

    _text_seq = 0

    def text_pgm(self, text, variant=0, w=240, h=40):
        TestLabels._text_seq += 1
        out = os.path.join(self.tmp, "text-%d.pgm" % TestLabels._text_seq)
        proc = subprocess.run([MM_HOST, "text", "--text", text,
                               "--variant", str(variant), "--width", str(w),
                               "--height", str(h), "--out", out],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        with open(out, "rb") as f:
            return f.read()

    def test_accented_letters_do_not_fall_back_to_question_mark(self):
        # Regression: the UTF-8 decoder classified the DECODED codepoint
        # against 0xf0, so every Latin-1 letter from 0xF0 up (ò, ù, ú, ü, ñ...)
        # was swallowed as a bogus 4-byte sequence and rendered as '?'.
        accent = self.text_pgm("Cantù")
        self.assertNotEqual(accent, self.text_pgm("Cant?"),
                            "ù must not fall back to '?'")
        self.assertNotEqual(accent, self.text_pgm("Cantu"))
        for ch, base in [("à", "a"), ("è", "e"), ("é", "e"), ("ì", "i"),
                         ("ò", "o"), ("ù", "u"), ("À", "A"), ("È", "E")]:
            self.assertNotEqual(self.text_pgm(ch), self.text_pgm(base), ch)

    def test_typographic_punctuation_normalises(self):
        # 3-byte sequences (’ “ ” –) must decode and normalise to ASCII
        self.assertEqual(self.text_pgm("Sant’Ambrogio", variant=1),
                         self.text_pgm("Sant'Ambrogio", variant=1))


# --------------------------------------------------------------------------- #
# Label layout cache (plan D5: world anchored set, stable between the fast and
# the grayscale pass; a strip repaint must not touch the POI sections)
# --------------------------------------------------------------------------- #

@unittest.skipUnless(have_tools(), "native tools cannot be built")
class TestLabelCache(VmapFixture):
    def cachetest(self, pan_x, pan_y, block_rebuild=False):
        out = os.path.join(self.tmp, "cache-a.pgm")
        out2 = os.path.join(self.tmp, "cache-b.pgm")
        cmd = [MM_HOST, "cachetest", "--input", self.vmap,
               "--lat", "45.7633", "--lon", "9.04", "--zoom", "13",
               "--width", "480", "--height", "583",
               "--pan-x", str(pan_x), "--pan-y", str(pan_y),
               "--out", out, "--out2", out2]
        if block_rebuild:
            cmd.append("--block-rebuild")
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        return proc.stdout

    def test_fresh_cache_matches_uncached_and_reuses_within_slack(self):
        text = self.cachetest(8, 0)
        # a fresh cache must be pixel identical to the uncached render
        self.assertIn("cache fresh mismatches: 0", text)
        # re-rendering the same viewport must not rebuild the layout
        self.assertIn("cache repeat rebuilds: 0", text)
        # a pan inside the slack reuses the layout (no POI reads, no rebuild)
        self.assertIn("cache pan(8,0) rebuilds: 0", text)
        m = re.search(r"cache pan\(8,0\) rebuilds: 0 labels: (\d+)", text)
        self.assertIsNotNone(m)
        self.assertGreater(int(m.group(1)), 0, "the cache must hold real labels")

    def test_pan_beyond_slack_rebuilds_bit_exact(self):
        text = self.cachetest(200, 0)
        self.assertIn("cache fresh mismatches: 0", text)
        self.assertIn("cache pan(200,0) rebuilds: 1", text)
        # past the slack the rebuild uses the normal placement rules, so the
        # cached render must match the uncached one pixel for pixel
        self.assertIn("pan uncached mismatches: 0", text)

    def test_strip_pan_never_rebuilds_mid_gesture(self):
        # D5 hysteresis: a strip repaint past the slack keeps the shifted set
        # instead of rebuilding it (no label pop while dragging)
        text = self.cachetest(200, 0, block_rebuild=True)
        self.assertIn("cache pan(200,0) rebuilds: 0", text)
        m = re.search(r"cache pan\(200,0\) rebuilds: 0 labels: (\d+)", text)
        self.assertIsNotNone(m)
        self.assertGreater(int(m.group(1)), 0, "the stale set must still be drawn")


# --------------------------------------------------------------------------- #
# Strip repaint (plan D5): a narrow clip must write exactly the pixels a full
# render writes inside it, and nothing outside
# --------------------------------------------------------------------------- #

@unittest.skipUnless(have_tools(), "native tools cannot be built")
class TestStripRepaint(VmapFixture):
    def stripcheck(self, lat, lon, z, rect):
        x0, y0, x1, y1 = rect
        proc = subprocess.run([MM_HOST, "stripcheck", "--input", self.vmap,
                               "--lat", str(lat), "--lon", str(lon),
                               "--zoom", str(z), "--width", "480", "--height", "583",
                               "--x0", str(x0), "--y0", str(y0),
                               "--x1", str(x1), "--y1", str(y1)],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("inside 0 outside 0", proc.stdout, proc.stdout)

    def test_strips_match_full_render_bit_exact(self):
        rects = [(0, 0, 24, 583), (456, 0, 480, 583), (0, 0, 480, 24),
                 (0, 559, 480, 583), (200, 200, 240, 240)]
        for z in (7.0, 9.0, 11.0, 12.0, 15.0):
            for r in rects:
                with self.subTest(z=z, rect=r):
                    self.stripcheck(45.7633, 9.04, z, r)


# --------------------------------------------------------------------------- #
# Fuzzing the reader (UBSan build: signed overflow / bad shifts in the fixed
# point decoder are exactly what the property tests must never see)
# --------------------------------------------------------------------------- #

@unittest.skipUnless(have_tools() and have_ubsan(),
                     "UBSan build of mm_host not available")
class TestFuzzReader(VmapFixture):
    def mutate_and_read(self, mutator, name):
        raw = bytearray(open(self.vmap, "rb").read())
        mutator(raw)
        path = os.path.join(self.tmp, "fuzz-%s.mmap" % name)
        with open(path, "wb") as f:
            f.write(raw)
        proc = subprocess.run([MM_HOST_UBSAN, "fuzzread", "--input", path],
                              capture_output=True, text=True, timeout=120)
        self.assertGreaterEqual(proc.returncode, 0, proc.stderr[-4000:])
        self.assertNotIn("runtime error", proc.stderr)

    def test_bit_flips_in_header_and_index(self):
        rng = random.Random(7)
        for i in range(8):
            def flip(raw, rng=rng):
                for _ in range(6):
                    pos = rng.randrange(0, min(len(raw), 4096))
                    raw[pos] ^= 1 << rng.randrange(8)
            self.mutate_and_read(flip, "flip%d" % i)

    def test_truncated_files(self):
        full = os.path.getsize(self.vmap)
        for frac in (0, 1, 128, 1000, full // 2, full - 1):
            def trunc(raw, frac=frac):
                del raw[frac:]
            self.mutate_and_read(trunc, "trunc%d" % frac)

    def test_garbage_section_table(self):
        def garbage(raw):
            rng = random.Random(99)
            for pos in range(128, min(len(raw), 128 + 96)):
                raw[pos] = rng.randrange(256)
        self.mutate_and_read(garbage, "sections")

    def test_bit_flips_in_payloads(self):
        rng = random.Random(21)
        size = os.path.getsize(self.vmap)
        for i in range(6):
            def flip(raw, rng=rng, size=size):
                for _ in range(200):
                    pos = rng.randrange(size // 2, len(raw))
                    raw[pos] ^= 1 << rng.randrange(8)
            self.mutate_and_read(flip, "payload%d" % i)

    def test_directory_on_mutated_payloads(self):
        """--deep also builds the record directories from the mutated bytes:
        the builder must stay inside the payload and trip no sanitizer."""
        rng = random.Random(33)
        size = os.path.getsize(self.vmap)
        for i in range(3):
            raw = bytearray(open(self.vmap, "rb").read())
            for _ in range(300):
                pos = rng.randrange(size // 2, len(raw))
                raw[pos] ^= 1 << rng.randrange(8)
            path = os.path.join(self.tmp, "fuzz-dir%d.mmap" % i)
            with open(path, "wb") as f:
                f.write(raw)
            proc = subprocess.run([MM_HOST_UBSAN, "fuzzread", "--input", path,
                                   "--deep"], capture_output=True, text=True,
                                  timeout=120)
            self.assertGreaterEqual(proc.returncode, 0, proc.stderr[-4000:])
            self.assertNotIn("runtime error", proc.stderr)


@unittest.skipUnless(have_tools() and have_ubsan(),
                     "UBSan build of mm_host not available")
class TestSanitisedRender(VmapFixture):
    """Renders real scenes under UBSan: fixed point code must be UB free.

    The fuzz tests only read the structure; this exercises the rasteriser
    (fill/stroke/text) on real geometry, which is where undefined behaviour
    like unaligned workspace partitions or negative shifts hides.
    """

    def test_render_under_ubsan(self):
        for i, (lat, lon, z) in enumerate([(45.85, 9.15, 12), (45.85, 9.15, 13.5),
                                           (45.85, 9.15, 15)]):
            out = os.path.join(self.tmp, "ubsan%d.pgm" % i)
            cmd = [MM_HOST_UBSAN, "render", "--input", self.vmap,
                   "--lat", str(lat), "--lon", str(lon), "--zoom", str(z),
                   "--width", "480", "--height", "583", "--out", out]
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            self.assertEqual(proc.returncode, 0,
                             "UBSan render failed: %s" % proc.stderr[-4000:])
            self.assertNotIn("runtime error", proc.stderr)

    def test_fill_rings_with_many_vertices(self):
        """Rings above the record cap are split by rect bisection: exercise it
        through a full generate + render cycle on the synthetic lake."""
        proc = subprocess.run([MM_HOST_UBSAN, "render", "--input", self.vmap,
                               "--lat", "45.82", "--lon", "9.12", "--zoom", "14",
                               "--width", "480", "--height", "583",
                               "--out", os.path.join(self.tmp, "ring.pgm")],
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr[-4000:])


# --------------------------------------------------------------------------- #
# Python <-> C cross check of the format
# --------------------------------------------------------------------------- #

@unittest.skipUnless(have_tools(), "native tools cannot be built")
class TestPythonCrossCheck(VmapFixture):
    def test_python_parses_the_same_structure(self):
        """Minimal from-scratch v2 parser: header + band + POI counts."""
        with open(self.vmap, "rb") as f:
            blob = f.read()
        magic, version, header_size, flags, tile_px, bits, zoom_count, \
            n_sections, table_off, file_size = struct.unpack_from("<8sHHIIBBHII", blob, 0)
        self.assertEqual(magic, b"MONOMMAP")
        self.assertEqual(version, 2)
        self.assertEqual(header_size, 128)
        self.assertEqual(file_size, len(blob))
        self.assertTrue(flags & (1 << 5), "FLAG_VECTOR must be set")
        style_version = flags >> 16
        self.assertGreaterEqual(style_version, 1)

        bands = []
        poi_count = strings_len = 0
        for i in range(n_sections):
            stype, off, size, aux = struct.unpack_from("<IIII", blob,
                                                       table_off + i * 16)
            self.assertLessEqual(off + size, len(blob))
            if stype == 5:  # SEC_VBAND
                band, grid_z, bflags = struct.unpack_from("<BBH", blob, off)
                x0, nx, y0, ny = struct.unpack_from("<IIII", blob, off + 4)
                present = struct.unpack_from("<I", blob, off + 40)[0]
                zmin, zmax = struct.unpack_from("<BB", blob, off + 44)
                bands.append((band, grid_z, zmin, zmax, nx, ny, present))
                self.assertEqual(aux, band)
            elif stype == 2:  # SEC_POI
                poi_count = struct.unpack_from("<I", blob, off)[0]
                self.assertEqual(poi_count, aux)
            elif stype == 3:
                strings_len = size

        proc = subprocess.run([VMAP, "info", "--input", self.vmap],
                              capture_output=True, text=True)
        out = proc.stdout
        self.assertEqual(len(bands), 3)
        for band, grid_z, zmin, zmax, nx, ny, present in bands:
            self.assertIn("band %d     : z%d-%d grid z%d %dx%d cells, %d present"
                          % (band, zmin, zmax, grid_z, nx, ny, present), out)
        self.assertIn("poi        : %u entries" % poi_count, out)

    def test_strings_are_valid_utf8(self):
        with open(self.vmap, "rb") as f:
            blob = f.read()
        n_sections = struct.unpack_from("<H", blob, 22)[0]
        table_off = struct.unpack_from("<I", blob, 24)[0]
        for i in range(n_sections):
            stype, off, size, aux = struct.unpack_from("<IIII", blob,
                                                       table_off + i * 16)
            if stype == 3:
                blob[off:off + size].decode("utf-8")  # must not raise


if __name__ == "__main__":
    unittest.main()
