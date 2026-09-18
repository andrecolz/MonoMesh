#!/usr/bin/env python3
"""Test-suite for the optimised MonoMesh tile generator.

Run with:  python3 -m unittest discover -s tools/tests -v
or:        python3 tools/tests/test_pipeline.py

The suite is self-contained: it generates a small synthetic .osm.pbf (via
pyosmium) so that everything runs in a couple of seconds, and it uses the
legacy renderer as an oracle to prove the fast path is pixel-identical.
"""

import gzip
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import osmium
from osmium.osm import mutable
from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
SCRIPT = os.path.join(TOOLS, "mmap_converter.py")


if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
# Plain import, never importlib: sys.modules must hold exactly one copy of the tool,
# otherwise the fork-based render pool cannot pickle its worker function ("it's not
# the same object as mmap_converter._render_one") when several test modules load it.
import mmap_converter as fot  # noqa: E402


# --------------------------------------------------------------------------- #
# Synthetic fixture
# --------------------------------------------------------------------------- #
def make_synthetic_pbf(path, span=0.30, lon0=9.00, lat0=45.70):
    """Deterministic synthetic map: grid of roads, rail, water, places.

    It also contains a lake modelled as a multipolygon *relation* (two outer
    ways plus an island ring) and a few tiny ponds, which is what the zoom
    dependent style has to handle.
    """
    nodes = []
    grid = 16
    node_id = 1
    ids = {}
    for i in range(grid):
        for j in range(grid):
            lon = lon0 + span * i / (grid - 1)
            lat = lat0 + span * j / (grid - 1)
            tags = {}
            # a few places on a diagonal, with varied types
            if i == j == 2:
                tags = {"place": "city", "name": "Citta Grande"}
            elif i == j == 6:
                tags = {"place": "town", "name": "Paese Medio"}
            elif i == 10 and j == 4:
                tags = {"place": "village", "name": "Villaggio"}
            elif i == 3 and j == 12:
                tags = {"place": "hamlet", "name": "Cascina"}
            ids[(i, j)] = node_id
            nodes.append((node_id, lon, lat, tags))
            node_id += 1

    ways = []
    way_id = 1000
    for i in range(grid):
        for j in range(grid):
            if i + 1 < grid:
                cls = "motorway" if i % 5 == 0 else ("primary" if i % 3 == 0 else "residential")
                ways.append((way_id, [ids[(i, j)], ids[(i + 1, j)]], {"highway": cls}))
                way_id += 1
            if j + 1 < grid:
                cls = "secondary" if j % 4 == 0 else "unclassified"
                ways.append((way_id, [ids[(i, j)], ids[(i, j + 1)]], {"highway": cls}))
                way_id += 1
    # railway
    ways.append((way_id, [ids[(0, 8)], ids[(grid - 1, 8)]], {"railway": "rail"}))
    way_id += 1
    # closed water polygon + a stream + a footway (never rendered)
    ways.append((way_id, [ids[(4, 4)], ids[(6, 4)], ids[(6, 6)], ids[(4, 6)], ids[(4, 4)]],
                 {"natural": "water", "name": "Lago Test"}))
    way_id += 1
    ways.append((way_id, [ids[(1, 1)], ids[(5, 3)], ids[(9, 5)]], {"waterway": "stream"}))
    way_id += 1
    ways.append((way_id, [ids[(2, 2)], ids[(3, 3)]], {"highway": "footway"}))
    way_id += 1
    ways.append((way_id, [ids[(7, 7)], ids[(8, 8)], ids[(9, 9)]], {"highway": "cycleway"}))
    way_id += 1

    writer = osmium.SimpleWriter(path)
    for nid, lon, lat, tags in nodes:
        writer.add_node(mutable.Node(id=nid, location=(lon, lat), tags=tags))
    for wid, refs, tags in ways:
        writer.add_way(mutable.Way(id=wid, nodes=refs, tags=tags))

    # --- lake as multipolygon relation (outer: two ways, inner: island) -----
    lake_id = 50000
    lake_nodes = [
        (lake_id + 0, lon0 + 0.100, lat0 + 0.100),
        (lake_id + 1, lon0 + 0.160, lat0 + 0.100),
        (lake_id + 2, lon0 + 0.160, lat0 + 0.160),
        (lake_id + 3, lon0 + 0.100, lat0 + 0.160),
        (lake_id + 4, lon0 + 0.125, lat0 + 0.125),
        (lake_id + 5, lon0 + 0.135, lat0 + 0.125),
        (lake_id + 6, lon0 + 0.135, lat0 + 0.135),
        (lake_id + 7, lon0 + 0.125, lat0 + 0.135),
    ]
    for nid, lon, lat in lake_nodes:
        writer.add_node(mutable.Node(id=nid, location=(lon, lat), tags={}))
    outer_a, outer_b = 60001, 60002
    inner_w = 60003
    writer.add_way(mutable.Way(id=outer_a, nodes=[lake_id, lake_id + 1, lake_id + 2], tags={}))
    writer.add_way(mutable.Way(id=outer_b, nodes=[lake_id + 2, lake_id + 3, lake_id], tags={}))
    writer.add_way(mutable.Way(id=inner_w, nodes=[lake_id + 4, lake_id + 5, lake_id + 6,
                                                 lake_id + 7, lake_id + 4], tags={}))
    writer.add_relation(mutable.Relation(
        id=70001,
        members=[osmium.osm.RelationMember(ref=outer_a, mtype="w", role="outer"),
                 osmium.osm.RelationMember(ref=outer_b, mtype="w", role="outer"),
                 osmium.osm.RelationMember(ref=inner_w, mtype="w", role="inner")],
        tags={"type": "multipolygon", "natural": "water", "water": "lake", "name": "Lago Test"}))

    # --- tiny ponds (must disappear at low zoom) and extra cities ----------
    pond_id = 51000
    for k in range(3):
        base = pond_id + k * 4
        lon = lon0 + 0.04 + k * 0.05
        lat = lat0 + 0.22
        for i, (dlon, dlat) in enumerate(((0, 0), (0.0004, 0), (0.0004, 0.0004), (0, 0.0004))):
            _ = dlon, dlat
            writer.add_node(mutable.Node(id=base + i, location=(lon + 0.0004 * (i in (1, 2)),
                                                               lat + 0.0004 * (i in (2, 3))),
                                         tags={}))
        writer.add_way(mutable.Way(id=61000 + k,
                                   nodes=[base, base + 1, base + 2, base + 3, base],
                                   tags={"natural": "water"}))
    for k in range(12):
        writer.add_node(mutable.Node(id=52000 + k,
                                     location=(lon0 + 0.02 + k * 0.022, lat0 + 0.28),
                                     tags={"place": "city", "name": f"Citta {k:02d}",
                                           "population": str(5000 * (k + 1))}))
    writer.close()
    return {
        "bbox": (lat0, lon0, lat0 + span, lon0 + span),
        "n_nodes": len(nodes),
        "n_ways": len(ways),
        "lake_center": (lat0 + 0.110, lon0 + 0.110),   # inside the lake, outside the island
        "island_center": (lat0 + 0.130, lon0 + 0.130),
    }


def reference_extract(pbf_path, min_lat, min_lon, max_lat, max_lon):
    """Naive extraction, mirroring the legacy implementation."""
    dlat = (max_lat - min_lat) * 0.15
    dlon = (max_lon - min_lon) * 0.15
    b_min_lat, b_max_lat = min_lat - dlat, max_lat + dlat
    b_min_lon, b_max_lon = min_lon - dlon, max_lon + dlon

    kf = osmium.filter.KeyFilter("highway", "waterway", "railway", "natural", "place")
    fp = osmium.FileProcessor(pbf_path).with_locations().with_filter(kf)
    ways, places = [], []
    for obj in fp:
        if obj.is_node() and "place" in obj.tags:
            loc = obj.location
            if loc.valid() and b_min_lat <= loc.lat <= b_max_lat and b_min_lon <= loc.lon <= b_max_lon:
                places.append((obj.tags.get("name", ""), loc.lat, loc.lon, obj.tags["place"]))
        elif obj.is_way():
            hw = obj.tags.get("highway")
            rw = obj.tags.get("railway")
            ww = obj.tags.get("waterway")
            nat = obj.tags.get("natural")
            if not (hw or rw or ww or nat == "water"):
                continue
            pts = []
            has_in = False
            for n in obj.nodes:
                if n.location.valid():
                    lat, lon = n.location.lat, n.location.lon
                    pts.append((round(lat, 5), round(lon, 5)))
                    if b_min_lat <= lat <= b_max_lat and b_min_lon <= lon <= b_max_lon:
                        has_in = True
            if has_in and len(pts) >= 2:
                closed = (pts[0] == pts[-1]) and len(pts) >= 4
                ways.append({
                    "class": hw or rw or ww or nat,
                    "type": "highway" if hw else ("railway" if rw else "water"),
                    "closed": closed,
                    "name": obj.tags.get("name", ""),
                    "pts": pts,
                })
    return ways, places


class SyntheticFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="monomesh-test-")
        cls.pbf = os.path.join(cls.tmp, "synthetic.osm.pbf")
        cls.info = make_synthetic_pbf(cls.pbf)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)


class TestExtraction(SyntheticFixture):
    def test_extraction_matches_reference(self):
        bbox = self.info["bbox"]
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        ways_ref, places_ref = reference_extract(self.pbf, *bbox)

        # footway/cycleway are dropped at extraction time by design
        ways_ref = [w for w in ways_ref if not (w["type"] == "highway" and w["class"] in fot.UNRENDERED_HIGHWAY)]
        got = data.to_legacy_dict()
        # lakes assembled from multipolygon relations are an *addition* of the
        # new pipeline (the naive reference only sees plain ways)
        rings = [w for w in got["ways"] if w.get("water_kind") in (fot.WATER_RING_OUTER,
                                                                   fot.WATER_RING_INNER)]
        got["ways"] = [w for w in got["ways"] if w not in rings]
        self.assertGreater(len(rings), 0, "nessun anello d'acqua assemblato dalle relation")
        self.assertEqual(len(got["ways"]), len(ways_ref))
        self.assertEqual(data.n_places, len(places_ref))
        # the cache only keeps the class for highway ways (water/railway classes
        # are never used by the renderer), so normalise those away
        def key(w):
            cls = w["class"] if w["type"] == "highway" else w["type"]
            return (w["type"], cls, tuple(w["pts"]), w["closed"])
        self.assertEqual(sorted(key(w) for w in got["ways"]),
                         sorted(key(w) for w in ways_ref))
        self.assertEqual({p["name"] for p in got["places"]},
                         {p[0] for p in places_ref})

    def test_extraction_drops_unrendered_highway_classes(self):
        bbox = self.info["bbox"]
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        classes = {data.way_class(i) for i in range(data.n_ways)}
        self.assertNotIn("footway", classes)
        self.assertNotIn("cycleway", classes)
        self.assertIn("residential", classes)


class TestCache(SyntheticFixture):
    def test_binary_cache_roundtrip(self):
        bbox = self.info["bbox"]
        path = os.path.join(self.tmp, "cache.mosmb")
        data = fot.extract_area_from_pbf(self.pbf, *bbox, cache_path=path, verbose=False)
        self.assertTrue(os.path.exists(path))
        loaded = fot.cache_read(path)
        self.assertEqual(loaded.n_ways, data.n_ways)
        self.assertEqual(loaded.n_places, data.n_places)
        self.assertEqual(list(loaded.bbox), list(data.bbox))
        self.assertEqual(list(loaded.lat_e5), list(data.lat_e5))
        self.assertEqual(list(loaded.lon_e5), list(data.lon_e5))
        self.assertEqual(loaded.place_names, data.place_names)
        self.assertEqual(loaded.to_legacy_dict(), data.to_legacy_dict())

    def test_legacy_json_cache_is_converted(self):
        bbox = self.info["bbox"]
        legacy = os.path.join(self.tmp, "legacy_area.json.gz")
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        with gzip.open(legacy, "wt", encoding="utf-8") as f:
            json.dump(data.to_legacy_dict(), f)
        loaded = fot.load_or_extract(self.pbf, bbox, "legacy_area",
                                     cache_dir=self.tmp, verbose=False)
        self.assertEqual(loaded.n_ways, data.n_ways)
        converted = [f for f in os.listdir(self.tmp) if f.startswith("legacy_area_") and f.endswith(".mosmb")]
        self.assertTrue(converted, "legacy cache was not converted to the binary format")

    def test_cache_is_stale_detects_other_bbox(self):
        bbox = self.info["bbox"]
        path = os.path.join(self.tmp, "stale.mosmb")
        data = fot.extract_area_from_pbf(self.pbf, *bbox, cache_path=path, verbose=False)
        self.assertFalse(fot.cache_is_stale(path, bbox))
        self.assertTrue(fot.cache_is_stale(path, (bbox[0] + 0.01, bbox[1], bbox[2], bbox[3])))


class TestOpenWaterRings(unittest.TestCase):
    """Water multipolygons truncated by the extracted area.

    Regression: such a ring used to be stored as "closed", so filling it made the
    rasteriser join the two loose ends with a straight edge - a black band across
    the map. Now the extraction marks a ring as fillable only when that edge stays
    outside the area, and the renderers outline the others.
    """

    BBOX = (45.0, 9.0, 45.5, 9.5)

    def test_segment_rect_intersection(self):
        # a segment that stays outside the rectangle never hits it
        self.assertFalse(fot._segment_hits_rect(45.51, 9.12, 45.52, 9.10, self.BBOX))
        # ... and one crossing the area does
        self.assertTrue(fot._segment_hits_rect(45.10, 9.05, 45.30, 9.45, self.BBOX))
        # touching the border counts as a hit
        self.assertTrue(fot._segment_hits_rect(45.50, 9.05, 45.50, 9.45, self.BBOX))

    def test_ring_can_be_filled(self):
        # really closed -> fillable
        self.assertTrue(fot._ring_can_be_filled([45.1, 45.2, 45.2, 45.1],
                                                [9.1, 9.1, 9.2, 9.1], self.BBOX))
        # truncated but the invented edge stays outside (lake cut by the map border)
        # (two banks going down into the area, both ends just above the north border)
        self.assertTrue(fot._ring_can_be_filled([45.51, 45.30, 45.30, 45.51],
                                                [9.10, 9.10, 9.12, 9.12], self.BBOX))
        # truncated across the whole area (riverbank relation) -> outline only
        self.assertFalse(fot._ring_can_be_filled([45.10, 45.20, 45.30], [9.05, 9.20, 9.45],
                                                 self.BBOX))

    def _data(self, fillable):
        """A lake-like rectangle, optionally truncated so that it does not close."""
        data = fot.VectorData(self.BBOX)
        n = 12
        lat0, lon0, lat1, lon1 = 45.10, 9.10, 45.20, 9.20
        pts = []
        pts += [(lat0 + (lat1 - lat0) * i / n, lon0) for i in range(n)]
        pts += [(lat1, lon0 + (lon1 - lon0) * i / n) for i in range(n)]
        pts += [(lat1 - (lat1 - lat0) * i / n, lon1) for i in range(n)]
        pts += [(lat0, lon1 - (lon1 - lon0) * i / n) for i in range(n)]
        if fillable:
            pts.append(pts[0])              # the ring really closes
        else:
            pts = pts[: 3 * n]              # three sides only: not fillable
        data.add_way(fot.TYPE_WATER, fot.WATER_RING_OUTER, fillable,
                     [int(round(p[0] * fot.E5)) for p in pts],
                     [int(round(p[1] * fot.E5)) for p in pts])
        return data

    def _render(self, fillable):
        z = 13
        # a tile on the lake's corner: both the filled and the outlined version
        # paint something there, so the ink comparison is meaningful
        tx, ty = fot.deg2num(45.105, 9.105, z)
        data = self._data(fillable)
        index = fot.build_tile_index(data, z, locations=fot.project_ways_at_zoom(data, z))
        legacy = data.to_legacy_dict()
        tmp = tempfile.mkdtemp(prefix="monomesh-ring-")
        try:
            ref_path = os.path.join(tmp, "ref.png")
            fast_path = os.path.join(tmp, "fast.png")
            fot.render_toner_tile_reference(z, tx, ty, legacy["ways"], legacy["places"],
                                            ref_path)
            fot.render_tile_fast(z, tx, ty, index, fast_path,
                                 save_kwargs={"compress_level": 6})
            return Image.open(ref_path).convert("L"), Image.open(fast_path).convert("L")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_fast_path_matches_the_reference(self):
        for fillable in (True, False):
            ref, fast = self._render(fillable)
            diff = sum(1 for v in ImageChops.difference(ref, fast).getdata() if v)
            self.assertEqual(diff, 0, f"riempibile={fillable}: {diff} pixel diversi")

    def test_ring_that_is_not_fillable_is_only_outlined(self):
        _, filled = self._render(True)
        _, outlined = self._render(False)
        ink_filled = sum(1 for v in filled.getdata() if v < 128)
        ink_outlined = sum(1 for v in outlined.getdata() if v < 128)
        self.assertGreater(ink_filled, 20000, "il lago riempibile deve essere pieno")
        self.assertLess(ink_outlined, ink_filled // 4,
                        "un anello non riempibile va contornato, non riempito")


class TestRenderParity(SyntheticFixture):
    def _render_both(self, z, tx, ty, style_dark=False):
        bbox = self.info["bbox"]
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        legacy = data.to_legacy_dict()
        locations = fot.project_ways_at_zoom(data, z)
        index = fot.build_tile_index(data, z, locations=locations)
        ref_path = os.path.join(self.tmp, f"ref_{z}_{tx}_{ty}.png")
        fast_path = os.path.join(self.tmp, f"fast_{z}_{tx}_{ty}.png")
        fot.render_toner_tile_reference(z, tx, ty, legacy["ways"], legacy["places"], ref_path,
                                        style_dark=style_dark)
        fot.render_tile_fast(z, tx, ty, index, fast_path, style_dark=style_dark,
                             save_kwargs={"compress_level": 6})
        return Image.open(ref_path).convert("L"), Image.open(fast_path).convert("L")

    def test_tiles_are_pixel_identical(self):
        bbox = self.info["bbox"]
        for z in (12, 13, 14):
            x0, y0 = fot.deg2num(bbox[2], bbox[1], z)
            x1, y1 = fot.deg2num(bbox[0], bbox[3], z)
            for tx in range(min(x0, x1), max(x0, x1) + 1):
                for ty in range(min(y0, y1), max(y0, y1) + 1):
                    ref, fast = self._render_both(z, tx, ty)
                    diff = ImageChops.difference(ref, fast)
                    n_diff = sum(1 for v in diff.getdata() if v)
                    self.assertEqual(n_diff, 0, f"tile {z}/{tx}/{ty} differs in {n_diff} pixels")

    def test_dark_style_is_pixel_identical(self):
        bbox = self.info["bbox"]
        z = 13
        tx, ty = fot.deg2num((bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2, z)
        ref, fast = self._render_both(z, tx, ty, style_dark=True)
        self.assertEqual(sum(1 for v in ImageChops.difference(ref, fast).getdata() if v), 0)

    def test_fast_renderer_touches_few_ways(self):
        """The whole point of the optimisation: small candidate sets per tile."""
        bbox = self.info["bbox"]
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        index = fot.build_tile_index(data, 13, locations=fot.project_ways_at_zoom(data, 13))
        tile = next(iter(index.buckets))
        self.assertLess(len(index.buckets[tile]), data.n_ways)


class TestStyleRules(SyntheticFixture):
    """Regression tests for the zoom dependent style and the lake support."""

    def test_lake_relation_is_filled_and_island_is_punched(self):
        bbox = self.info["bbox"]
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        kinds = {(_t, _p) for _i, _t, _p, _c, _o, _n in data.iter_ways()
                 if _t == fot.TYPE_WATER}
        self.assertIn((fot.TYPE_WATER, fot.WATER_RING_OUTER), kinds)
        self.assertIn((fot.TYPE_WATER, fot.WATER_RING_INNER), kinds)

        z = 16
        idx = fot.build_tile_index(data, z, locations=fot.project_ways_at_zoom(data, z))

        def render(lat, lon):
            tx, ty = fot.deg2num(lat, lon, z)
            out = os.path.join(self.tmp, f"lake_{z}_{tx}_{ty}.png")
            fot.render_tile_fast(z, tx, ty, idx, out, save_kwargs={"compress_level": 6})
            return Image.open(out).convert("L"), tx, ty

        def pixel_of(im, tx, ty, lat, lon):
            n = 2.0 ** z
            x = int(round((fot.world_x(lat, lon, n) - tx) * 256.0))
            y = int(round((fot.world_y(lat, lon, n) - ty) * 256.0))
            return x, y

        # a point in the lake, away from the island: must be black (filled)
        lake_lat, lake_lon = self.info["lake_center"]
        im, tx, ty = render(lake_lat, lake_lon)
        x, y = pixel_of(im, tx, ty, lake_lat, lake_lon)
        self.assertLess(im.getpixel((x, y)), 128, "il lago (relation) non e riempito")

        # the island ring must be punched back to the background colour
        isl_lat, isl_lon = self.info["island_center"]
        im2, tx2, ty2 = render(isl_lat, isl_lon)
        xi, yi = pixel_of(im2, tx2, ty2, isl_lat, isl_lon)
        self.assertGreater(im2.getpixel((xi, yi)), 128, "l'isola non e stata sbiancata")
        xo, yo = pixel_of(im2, tx2, ty2, isl_lat + 0.008, isl_lon + 0.008)
        if 0 <= xo < 256 and 0 <= yo < 256:
            self.assertLess(im2.getpixel((xo, yo)), 128, "l'acqua attorno all'isola manca")

    def test_low_zoom_draws_only_major_classes(self):
        bbox = self.info["bbox"]
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        idx = fot.build_tile_index(data, 9, locations=fot.project_ways_at_zoom(data, 9))
        drawn = set()
        for items in idx.buckets.values():
            for item in items:
                drawn.add(item[3])
        allowed = {"motorway", "trunk", "motorway_link", "trunk_link", "primary",
                   "primary_link", "water", "water_inner", "railway"}
        self.assertTrue(drawn <= allowed, f"classi non attese a z9: {drawn - allowed}")
        self.assertNotIn("residential", drawn)
        self.assertNotIn("service", drawn)

    def test_tiny_water_bodies_are_dropped_at_low_zoom(self):
        bbox = self.info["bbox"]
        data = fot.extract_area_from_pbf(self.pbf, *bbox, verbose=False)
        idx9 = fot.build_tile_index(data, 9, locations=fot.project_ways_at_zoom(data, 9))
        # the three ponds are far smaller than 5 px at z9: they must be gone
        water_items = [it for items in idx9.buckets.values() for it in items
                       if it[3] in ("water", "water_inner") and it[4] == 0]
        sizes = []
        for it in water_items:
            _prio, off, cnt, _cls, _w, _c, _blk = it
            xs = [idx9.xs[j] for j in range(off, off + cnt)]
            ys = [idx9.ys[j] for j in range(off, off + cnt)]
            sizes.append(max(max(xs) - min(xs), max(ys) - min(ys)) * 256.0)
        self.assertTrue(all(s >= fot.water_min_size(9) for s in sizes),
                        f"poligoni d'acqua troppo piccoli ancora presenti: {sizes}")

    def test_label_budget_and_collision(self):
        z = 9
        tx, ty = 0, 256   # equator, so that world_y() lands exactly in row 256
        n = 2.0 ** z
        entries = []
        for i in range(12):
            lat = 0.0
            lon = (tx + 0.05 + i * 0.03) / n * 360.0 - 180.0
            kind = 0                      # city
            entries.append((lat, lon, kind, f"City {i:02d}", 1000 * (i + 1)))

        class Recorder:
            def __init__(self):
                self.texts = []

            def text(self, xy, text, **kw):
                self.texts.append(text)

            def textbbox(self, xy, text, **kw):
                return (xy[0], xy[1], xy[0] + 9 * len(text), xy[1] + 14)

            def ellipse(self, xy, **kw):
                pass

        rec = Recorder()
        fot.draw_labels(rec, z, tx, ty, entries, 0, 255)
        self.assertLessEqual(len(rec.texts), fot.label_budget(z))
        self.assertGreater(len(rec.texts), 0)
        # the most populated city must win the contest for the space
        self.assertIn("City 11", rec.texts)

    def test_zoom_policies(self):
        self.assertNotIn("residential", fot.zoom_road_classes(9))
        self.assertIn("residential", fot.zoom_road_classes(13))
        self.assertIn("path", fot.zoom_road_classes(15))
        self.assertLess(fot.label_budget(9), fot.label_budget(14))
        self.assertGreater(fot.water_min_size(9), fot.water_min_size(14))
        self.assertFalse(fot.water_kind_drawn(fot.WATER_STREAM, 12))
        self.assertTrue(fot.water_kind_drawn(fot.WATER_STREAM, 13))


class TestPipeline(SyntheticFixture):
    def test_generate_skip_and_force(self):
        bbox = self.info["bbox"]
        out = os.path.join(self.tmp, "tiles")
        cache_dir = os.path.join(self.tmp, "cache")
        res = fot.generate_tiles_from_pbf(self.pbf, *bbox, [12, 13], output_dir=out,
                                          area_name="synth", force=True, jobs=2,
                                          cache_dir=cache_dir, verbose=False)
        self.assertGreater(res["rendered"], 0)
        for root, _dirs, files in os.walk(out):
            for f in files:
                self.assertTrue(f.endswith(".png"))
                with Image.open(os.path.join(root, f)) as im:
                    self.assertEqual(im.size, (256, 256))

        # second run: everything already present -> nothing rendered
        res2 = fot.generate_tiles_from_pbf(self.pbf, *bbox, [12, 13], output_dir=out,
                                           area_name="synth", force=False, jobs=2,
                                           cache_dir=cache_dir, verbose=False)
        self.assertEqual(res2["rendered"], 0)
        self.assertEqual(res2["skipped"], res["rendered"])

    def test_png_modes(self):
        bbox = self.info["bbox"]
        for mode in ("fast", "small", "mono"):
            out = os.path.join(self.tmp, f"tiles_{mode}")
            fot.generate_tiles_from_pbf(self.pbf, *bbox, [13], output_dir=out,
                                        area_name=f"synth_{mode}", force=True, jobs=1,
                                        png_mode=mode, verbose=False)
            found = []
            for root, _d, files in os.walk(out):
                found += [os.path.join(root, f) for f in files]
            self.assertTrue(found, f"no tiles produced for mode {mode}")
            with Image.open(found[0]) as im:
                if mode == "mono":
                    self.assertEqual(im.mode, "1")
                else:
                    self.assertEqual(im.mode, "L")

    def test_cli_smoke(self):
        bbox = self.info["bbox"]
        out = os.path.join(self.tmp, "tiles_cli")
        cmd = [sys.executable, SCRIPT, "--pbf", self.pbf,
               "--bbox", *[str(v) for v in bbox],
               "--zooms", "12", "--output", out, "--jobs", "1", "--quiet",
               "--format", "png",
               "--cache-dir", os.path.join(self.tmp, "cache_cli")]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        pngs = []
        for root, _d, files in os.walk(out):
            pngs += [f for f in files if f.endswith(".png")]
        self.assertGreater(len(pngs), 0)

    def test_container_temp_cleaned_on_render_failure(self):
        """A run that dies mid-render must not leave a payload temp behind.

        The old code left multi-hundred-MB .mmap-payload-*.tmp files next to the
        maps after every crash.
        """
        bbox = self.info["bbox"]
        out = os.path.join(self.tmp, "tiles_fail")
        cache_dir = os.path.join(self.tmp, "cache_fail")
        original = fot._render_one

        def boom(_task):
            raise RuntimeError("simulated render failure")

        fot._render_one = boom
        try:
            with self.assertRaises(RuntimeError):
                fot.generate_tiles_from_pbf(self.pbf, *bbox, [13], output_dir=out,
                                            area_name="synth_fail", force=True, jobs=1,
                                            cache_dir=cache_dir, verbose=False)
        finally:
            fot._render_one = original
        leftovers = [f for f in os.listdir(out) if f.startswith(".mmap-payload")]
        self.assertEqual(leftovers, [], f"temp rimasti: {leftovers}")


class TestRealDataParity(unittest.TestCase):
    """Parity check against the real (large) dataset, when present."""

    LEGACY_CACHE = os.environ.get(
        "MONOMESH_LEGACY_CACHE",
        os.path.join(os.path.dirname(TOOLS), ".pbf_cache", "mariano_comense_r20km.json.gz"),
    )

    def setUp(self):
        if not os.path.exists(self.LEGACY_CACHE):
            self.skipTest("real legacy cache not available")
        self.tmp = tempfile.mkdtemp(prefix="monomesh-real-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_real_tiles_are_pixel_identical(self):
        data = fot.cache_load_legacy_json(self.LEGACY_CACHE)
        legacy = data.to_legacy_dict()
        z = 14
        locations = fot.project_ways_at_zoom(data, z)
        index = fot.build_tile_index(data, z, locations=locations)
        bbox = data.bbox
        x0, y0 = fot.deg2num(bbox[2], bbox[1], z)
        x1, y1 = fot.deg2num(bbox[0], bbox[3], z)
        total_diff = 0
        total_px = 0
        for tx in range(min(x0, x1) + 4, min(x0, x1) + 7):
            for ty in range(min(y0, y1) + 4, min(y0, y1) + 6):
                ref_path = os.path.join(self.tmp, f"r_{tx}_{ty}.png")
                fast_path = os.path.join(self.tmp, f"f_{tx}_{ty}.png")
                fot.render_toner_tile_reference(z, tx, ty, legacy["ways"], legacy["places"], ref_path)
                fot.render_tile_fast(z, tx, ty, index, fast_path, save_kwargs={"compress_level": 6})
                ref = Image.open(ref_path).convert("L")
                fast = Image.open(fast_path).convert("L")
                total_diff += sum(1 for v in ImageChops.difference(ref, fast).getdata() if v)
                total_px += 256 * 256
        ratio = total_diff / total_px
        self.assertLess(ratio, 0.0005, f"{total_diff}/{total_px} pixels differ ({ratio:.4%})")


if __name__ == "__main__":
    unittest.main(verbosity=2)
