#!/usr/bin/env python3
"""Round-trip tests for the .mmap container (tools/mono_map.py).

Run with:  python3 -m unittest discover -s tools/tests -v
or:        python3 tools/tests/test_mmap_format.py

Every test writes a container with MapWriter and reads it back with MapReader,
so writer and reader can only drift apart by breaking one of these.
"""

import hashlib
import os
import shutil
import struct
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


class TestDeterministicContainer(MmapFixture):
    """The container bytes must not depend on the order the tiles were produced."""

    def _write(self, path, order):
        w = mm.MapWriter(path, bits=1, tile_px=TILE_PX, name="det", bbox=BBOX)
        for z, x, y, seed in order:
            if seed is None:
                w.add_tile_compressed(z, x, y, b"", blank=1)
            else:
                w.add_tile_compressed(z, x, y, zlib.compress(packed_tile(seed), 6))
        w.add_poi(45.25, 9.25, "city", 12345, "Citta Test")
        w.close()

    def test_same_tiles_same_bytes_whatever_the_order(self):
        tiles = [(12, 100, 200, 1), (12, 101, 200, 2), (12, 100, 201, None),
                 (13, 100, 200, 3), (13, 101, 200, 4), (13, 100, 201, 5)]
        a = os.path.join(self.tmp, "a.mmap")
        b = os.path.join(self.tmp, "b.mmap")
        self._write(a, tiles)
        self._write(b, list(reversed(tiles)))   # e.g. a different worker schedule

        def sha(path):
            with open(path, "rb") as fh:
                return hashlib.sha256(fh.read()).hexdigest()

        self.assertEqual(sha(a), sha(b))

        r = mm.MapReader(a)
        info = r.zooms[12]
        entries = [struct.unpack_from("<IHH", info["index"], i * 8)
                   for i in range(info["present"])]
        # rank order is row-major: payload offsets must grow with (y, x)
        offsets = [entry[0] for entry in entries if entry[1]]
        self.assertEqual(offsets, sorted(offsets))
        self.assertEqual(r.tile(12, 101, 200)[0], packed_tile(2))


class TestDenseTileStorage(MmapFixture):
    """The dense grid behind the tile_grids hint must not change the output."""

    def _write(self, path, tiles, tile_grids=None):
        w = mm.MapWriter(path, bits=1, tile_px=TILE_PX, name="dense", bbox=BBOX,
                         tile_grids=tile_grids)
        for z, x, y, seed in tiles:
            if seed is None:
                w.add_tile_compressed(z, x, y, b"", blank=2)
            else:
                w.add_tile_compressed(z, x, y, zlib.compress(packed_tile(seed), 6))
        return w.close()

    def test_dense_and_dict_agree_byte_for_byte(self):
        tiles = [(13, 100, 200, 1), (13, 101, 200, 2), (13, 100, 201, None),
                 (14, 400, 800, 3)]
        dict_path = os.path.join(self.tmp, "dict.mmap")
        dense_path = os.path.join(self.tmp, "dense.mmap")
        self._write(dict_path, tiles)
        self._write(dense_path, tiles, tile_grids={13: (100, 200, 2, 2),
                                                   14: (400, 800, 1, 1)})
        with open(dict_path, "rb") as fa, open(dense_path, "rb") as fb:
            self.assertEqual(fa.read(), fb.read())

    def test_grid_grows_for_out_of_hint_tile(self):
        path = os.path.join(self.tmp, "grow.mmap")
        tiles = [(13, 100, 200, 1), (13, 102, 200, 5)]   # the second is outside the hint
        self._write(path, tiles, tile_grids={13: (100, 200, 1, 1)})
        r = mm.MapReader(path)
        self.assertEqual(r.tile(13, 102, 200)[0], packed_tile(5))
        info = r.zooms[13]
        self.assertEqual((info["x0"], info["nx"]), (100, 3))


class TestMerge(MmapFixture):
    """merge: verbatim payload copy, conflict priority, flags and POIs."""

    def _container(self, path, tiles, pois=()):
        w = mm.MapWriter(path, bits=1, tile_px=TILE_PX, name="part", bbox=BBOX)
        for z, x, y, seed in tiles:
            if seed is None:
                w.add_tile_compressed(z, x, y, b"", blank=1)
            elif seed == "black":
                w.add_tile_compressed(z, x, y, b"", blank=2)
            else:
                w.add_tile_compressed(z, x, y, zlib.compress(packed_tile(seed), 6))
        for lat, lon, kind, pop, name in pois:
            w.add_poi(lat, lon, kind, pop, name)
        w.close()

    def test_merge_copies_and_resolves_conflicts(self):
        a = os.path.join(self.tmp, "a.mmap")
        b = os.path.join(self.tmp, "b.mmap")
        out = os.path.join(self.tmp, "merged.mmap")
        self._container(a, [(13, 100, 200, 1), (13, 101, 200, 2), (13, 100, 201, None)],
                        pois=[(45.1, 9.1, "city", 10, "Alfa")])
        self._container(b, [(13, 101, 200, 7), (13, 200, 300, "black"), (14, 1, 1, 3)],
                        pois=[(45.2, 9.2, "town", 20, "Beta")])
        stats = mm.merge_containers([a, b], out, verbose=False)
        self.assertEqual(stats["tiles"], 5)      # 3 + 3 - 1 overlap
        self.assertEqual(stats["pois"], 2)

        r = mm.MapReader(out)
        self.assertEqual(r.bits, 1)
        self.assertEqual(r.zooms[13]["present"], 4)
        # default priority=last: the overlapping tile comes from b
        self.assertEqual(r.tile(13, 101, 200)[0], packed_tile(7))
        self.assertEqual(r.tile(13, 100, 200)[0], packed_tile(1))
        # blank flags survive with their colour
        self.assertEqual(r.tile(13, 100, 201), (None, mm.TILE_FLAG_BLANK))
        self.assertEqual(r.tile(13, 200, 300),
                         (None, mm.TILE_FLAG_BLANK | mm.TILE_FLAG_BLANK_BLACK))
        self.assertEqual(r.tile(14, 1, 1)[0], packed_tile(3))
        self.assertEqual([p["name"] for p in r.pois], ["Alfa", "Beta"])
        self.assertEqual(mm.main(["verify", out]), 0)

    def test_priority_first_keeps_the_first_container(self):
        a = os.path.join(self.tmp, "a.mmap")
        b = os.path.join(self.tmp, "b.mmap")
        out = os.path.join(self.tmp, "merged.mmap")
        self._container(a, [(13, 100, 200, 1)])
        self._container(b, [(13, 100, 200, 7)])
        mm.merge_containers([a, b], out, priority="first", verbose=False)
        self.assertEqual(mm.MapReader(out).tile(13, 100, 200)[0], packed_tile(1))

    def test_bits_mismatch_is_rejected(self):
        a = os.path.join(self.tmp, "a.mmap")
        c = os.path.join(self.tmp, "c.mmap")
        out = os.path.join(self.tmp, "merged.mmap")
        self._container(a, [(13, 100, 200, 1)])
        w = mm.MapWriter(c, bits=2, tile_px=TILE_PX, name="two", bbox=BBOX)
        w.add_tile_compressed(13, 100, 200, b"", blank=1)
        w.close()
        with self.assertRaises(SystemExit):
            mm.merge_containers([a, c], out, verbose=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
