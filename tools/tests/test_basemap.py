#!/usr/bin/env python3
"""Tests for the Natural Earth basemap generator.

No network needed: the fixture builds a tiny synthetic shapefile (a half-world
land polygon and a border line) and checks the container the firmware reads.
"""

import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import basemap_generator as bg  # noqa: E402
import mono_map as mm  # noqa: E402


def land_rectangle(lon0, lat0, lon1, lat1):
    """A closed rectangle as a single-part shape (points, parts)."""
    return ([(lon0, lat0), (lon1, lat0), (lon1, lat1), (lon0, lat1), (lon0, lat0)], [0])


class TestBasemap(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="monomesh-basemap-")
        self.out = os.path.join(self.tmp, "basemap.mmap")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_land_sea_corners_and_blank_classes(self):
        # Land covers lon 45..185, lat -80..80: tile z2 (2, 2) (lon 0..90,
        # lat 0..-66) is half sea, half land, with the coastline mid-tile.
        bg.build_basemap([land_rectangle(45.0, -80.0, 185.0, 80.0)], [],
                         self.out, zoom_levels=[2], verbose=False)
        r = mm.MapReader(self.out)
        self.assertEqual(r.name, "basemap")

        payload, _flags = r.tile(2, 2, 2)
        self.assertIsNotNone(payload, "la tile costiera non deve essere blank")
        px = mm.unpack_tile(payload, r.tile_px, r.bits)

        def at(x, y):
            return px[y * 256 + x]

        self.assertEqual(at(5, 5), 0)       # oceano in alto a sinistra
        self.assertEqual(at(250, 5), 255)   # terra in alto a destra
        self.assertEqual(at(5, 250), 0)     # oceano in basso a sinistra
        self.assertEqual(at(250, 250), 255)

        # a fully oceanic tile is a black blank, a fully land one a white blank
        self.assertEqual(r.tile(2, 0, 0),
                         (None, mm.TILE_FLAG_BLANK | mm.TILE_FLAG_BLANK_BLACK))
        self.assertEqual(r.tile(2, 3, 2), (None, mm.TILE_FLAG_BLANK))

    def test_border_marks_land_tile(self):
        land = [land_rectangle(45.0, -80.0, 185.0, 80.0)]
        border = ([(60.0, 25.0), (150.0, 25.0)], [0])   # hairline at lat 25
        bg.build_basemap(land, [border], self.out, zoom_levels=[2], verbose=False)
        r = mm.MapReader(self.out)
        payload, _flags = r.tile(2, 3, 1)   # lon 90..180, lat 0..66
        self.assertIsNotNone(payload)
        px = mm.unpack_tile(payload, r.tile_px, r.bits)
        self.assertIn(0, set(px), "il confine deve lasciare pixel neri sulla terra")
        self.assertIn(255, set(px))

    def test_shapefile_roundtrip(self):
        import shapefile  # pyshp, the same reader the generator uses

        shp_path = os.path.join(self.tmp, "land")
        writer = shapefile.Writer(shp_path, shapeType=shapefile.POLYGON)
        writer.field("name", "C")
        writer.poly([[(45.0, -80.0), (185.0, -80.0), (185.0, 80.0),
                      (45.0, 80.0), (45.0, -80.0)]])
        writer.record("half")
        writer.close()
        shapes = bg.load_shapes(shp_path + ".shp")
        self.assertEqual(len(shapes), 1)
        bg.build_basemap(shapes, [], self.out, zoom_levels=[1, 2], verbose=False)
        r = mm.MapReader(self.out)
        self.assertEqual(sorted(r.zooms), [1, 2])
        self.assertEqual(mm.main(["verify", self.out]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
