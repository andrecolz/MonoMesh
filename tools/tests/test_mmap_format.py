#!/usr/bin/env python3
"""Round-trip tests for the .mmap container (tools/mono_map.py).

Run with:  python3 -m unittest discover -s tools/tests -v
or:        python3 tools/tests/test_mmap_format.py

Every test writes a container with MapWriter and reads it back with MapReader,
so writer and reader can only drift apart by breaking one of these.
"""

import os
import shutil
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import mono_map as mm  # noqa: E402


TILE_PX = 256
BBOX = (45.0, 9.0, 45.5, 9.5)


def packed_tile(seed=0):
    """Deterministic 1 bit/pixel payload (TILE_PX * TILE_PX / 8 bytes)."""
    n = TILE_PX * TILE_PX // 8
    return bytes(((i * 7 + seed) ^ (i >> 8)) & 0xFF for i in range(n))


class MmapFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="monomesh-mmap-")
        self.path = os.path.join(self.tmp, "test.mmap")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def writer(self):
        return mm.MapWriter(self.path, bits=1, tile_px=TILE_PX, name="test", bbox=BBOX)


class TestMmapRoundTrip(MmapFixture):
    """Writer -> reader: header, indexes, per-tile flags and tile content."""

    def test_round_trip(self):
        payload = packed_tile()
        compressed = zlib.compress(payload, 6)
        w = self.writer()
        w.add_tile_bits(13, 100, 200, payload)              # tile con contenuto
        w.add_tile_compressed(13, 101, 200, b"", blank=1)   # blank bianco
        w.add_tile_compressed(13, 100, 201, b"", blank=2)   # blank nero
        w.add_poi(45.25, 9.25, "city", 12345, "Citta Test")
        stats = w.close()

        # writer statistics describe the container that was written
        self.assertEqual(stats["tiles"], 3)
        self.assertEqual(stats["blank"], 2)
        self.assertEqual(stats["zooms"], {13: 3})
        self.assertEqual(stats["payload_bytes"], len(compressed))
        self.assertEqual(stats["pois"], 1)

        r = mm.MapReader(self.path)
        # header
        self.assertEqual(r.version, mm.VERSION)
        self.assertEqual(r.header_size, mm.HEADER_SIZE)
        self.assertEqual(r.tile_px, TILE_PX)
        self.assertEqual(r.bits, 1)
        self.assertEqual(r.name, "test")
        self.assertEqual(r.attribution, mm.ATTRIBUTION)
        self.assertTrue(r.flags & mm.FLAG_DEFLATE)
        self.assertTrue(r.flags & mm.FLAG_POI)
        self.assertEqual(r.declared_size, r.file_size)
        for got, want in zip(r.bbox, BBOX):
            self.assertAlmostEqual(got, want, places=6)

        # indexes
        self.assertEqual(sorted(r.zooms), [13])
        info = r.zooms[13]
        self.assertEqual((info["x0"], info["nx"], info["y0"], info["ny"]), (100, 2, 200, 2))
        self.assertEqual(info["present"], 3)
        self.assertEqual(info["payload_size"], len(compressed))
        self.assertEqual(info["prefix"], (0, 2, 3))          # cumulato per riga
        self.assertEqual(info["presence"], b"\xe0")   # tre tile presenti, MSB first
        self.assertEqual(r.stats()[13], {"tiles": 3, "grid": (2, 2),
                                         "payload_bytes": len(compressed)})
        # no dead bytes between the sections and the payload area
        self.assertEqual(stats["bytes"], info["payload_off"] + info["payload_size"])

        # per-tile flags and content
        got, flags = r.tile(13, 100, 200)
        self.assertEqual(flags, 0)
        self.assertEqual(got, payload)                # decompressed content
        self.assertEqual(r.tile(13, 101, 200), (None, mm.TILE_FLAG_BLANK))
        self.assertEqual(r.tile(13, 100, 201),
                         (None, mm.TILE_FLAG_BLANK | mm.TILE_FLAG_BLANK_BLACK))
        # in-grid but absent, outside the grid, unknown zoom
        self.assertEqual(r.tile(13, 101, 201), (None, 0))
        self.assertEqual(r.tile(13, 99, 200), (None, 0))
        self.assertEqual(r.tile(12, 100, 200), (None, 0))

        # POI section
        self.assertEqual(len(r.pois), 1)
        poi = r.pois[0]
        self.assertEqual(poi["name"], "Citta Test")
        self.assertEqual(poi["kind"], "city")
        self.assertEqual(poi["population"], 12345)
        self.assertAlmostEqual(poi["lat"], 45.25, places=6)
        self.assertAlmostEqual(poi["lon"], 9.25, places=6)


class TestEmptyEntry(MmapFixture):
    """Regression: an entry with length 0 and no blank flag must not crash."""

    def test_empty_entry_is_blank_not_a_crash(self):
        # add_tile_bits with an empty payload stores a zero length entry without
        # the blank flag; the reader must treat it as blank like the firmware
        # (map_pack.cpp) instead of zlib.decompress(b"").
        w = self.writer()
        w.add_tile_bits(13, 100, 200, b"")
        stats = w.close()
        self.assertEqual(stats["tiles"], 1)

        r = mm.MapReader(self.path)
        self.assertEqual(r.zooms[13]["present"], 1)
        self.assertEqual(r.tile(13, 100, 200), (None, 0))


class TestDuplicateTile(MmapFixture):
    """Regression: the same (z, x, y) added twice replaces, never doubles."""

    def test_duplicate_tile_replaces_without_double_counting(self):
        first, second = packed_tile(1), packed_tile(2)
        c_first, c_second = zlib.compress(first, 6), zlib.compress(second, 6)
        w = self.writer()
        w.add_tile_compressed(13, 100, 200, c_first)
        w.add_tile_compressed(13, 100, 200, c_second)      # sostituisce il precedente
        w.add_tile_compressed(13, 101, 200, b"", blank=1)
        stats = w.close()

        self.assertEqual(stats["tiles"], 2)                # non 3
        self.assertEqual(stats["blank"], 1)
        self.assertEqual(stats["zooms"], {13: 2})
        self.assertEqual(stats["payload_bytes"], len(c_second))

        r = mm.MapReader(self.path)
        info = r.zooms[13]
        self.assertEqual(info["present"], 2)
        self.assertEqual(info["payload_size"], len(c_second))
        self.assertEqual(info["prefix"], (0, 2))            # una riga, due tile
        self.assertEqual(stats["bytes"], info["payload_off"] + info["payload_size"])
        got, flags = r.tile(13, 100, 200)
        self.assertEqual(flags, 0)
        self.assertEqual(got, second)                      # l'ultima versione vince

    def test_duplicate_blank_does_not_count_twice(self):
        w = self.writer()
        w.add_tile_compressed(13, 100, 200, b"", blank=1)
        w.add_tile_compressed(13, 100, 200, b"", blank=2)  # bianco -> nero
        stats = w.close()

        self.assertEqual(stats["tiles"], 1)
        self.assertEqual(stats["blank"], 1)
        self.assertEqual(stats["zooms"], {13: 1})
        r = mm.MapReader(self.path)
        self.assertEqual(r.zooms[13]["present"], 1)
        self.assertEqual(r.tile(13, 100, 200),
                         (None, mm.TILE_FLAG_BLANK | mm.TILE_FLAG_BLANK_BLACK))

    def test_duplicate_larger_payload_leaves_no_double_count(self):
        small, big = packed_tile(1), packed_tile(2) + packed_tile(3)
        c_small, c_big = zlib.compress(small, 6), zlib.compress(big, 6)
        w = self.writer()
        w.add_tile_compressed(13, 100, 200, c_small)
        w.add_tile_compressed(13, 100, 200, c_big)         # piu' grande dello slot
        stats = w.close()

        self.assertEqual(stats["tiles"], 1)
        self.assertEqual(stats["zooms"], {13: 1})
        self.assertEqual(stats["payload_bytes"], len(c_big))
        r = mm.MapReader(self.path)
        info = r.zooms[13]
        self.assertEqual(info["present"], 1)
        self.assertEqual(info["payload_size"], len(c_big))
        self.assertEqual(stats["bytes"], info["payload_off"] + info["payload_size"])
        self.assertEqual(r.tile(13, 100, 200), (big, 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
