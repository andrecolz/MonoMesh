#!/usr/bin/env python3
"""
MonoMesh OSM PBF Tile Generator & Toner E-Paper Optimizer (fast edition)
Extracts geographic data from local OSM .pbf files (e.g. italy-260912.osm.pbf)
and renders crisp, high-contrast B/W Toner tiles (256x256 PNG) for M5Paper Mono.

Features:
- 100% Offline: Generates tiles locally directly from .osm.pbf
- Interactive TUI: Search city by name, custom radius, zoom selection
- Smart Caching: Saves extracted area vectors in a compact binary cache
- Authentic Toner Style: White land, black water, hierarchic roads with casing,
  and town/city labels with protective white halo for E-Paper display.
- Parallel rendering: one worker per CPU core (multiprocessing)
- Fast path renderer: ways are bucketed per tile once per zoom, so each tile
  only touches the geometry it actually shows.
- Off-grid tricks that keep the output pixel-identical but cut the work:
  * the projection only touches the ways the requested zooms can draw, and the
    per-way bounding box is measured once for all of them instead of once per
    zoom (the tile index of a z9-11 run costs a fourth of what it did);
  * the extraction reads way geometry from the C++ WKB factory in one call
    instead of resolving the location of every node in Python (~1.5x on the
    whole extraction of the north-west PBF);
  * long ways carry per-block bounding boxes, so a tile converts only the slice
    of the way it can see;
  * coordinates go to Pillow as a flat list, which costs one int per coordinate
    instead of a tuple per point.

Performance notes (measured on nord-ovest-260912.osm.pbf, 563 MB):
- legacy renderer: ~1525 ms/tile
- whole PBF z9-11 (1.463 tile): 10.4 s with a warm cache, 71 s cold (before);
  6.1 s warm, 44 s cold (after)
- whole PBF z12-14 (83.638 tile): 61.7 s -> 48.6 s (warm cache)
- whole PBF z8-15 (337.453 tile, 169 MB container): 198.8 s -> 177.4 s (warm cache)
- whole PBF z7-16 (1.345.815 tile, 412 MB container): 814 s with 13 workers
  (33 s index, 778 s render, 1.8 s container) and a ~1.2 GB peak of unique
  memory.

Memory notes.  The per-zoom index is stored in a compact CSR layout: a dense
grid of per-tile offsets plus one flat int32 array of way ids, already in
drawing order, and the per-way style lives in byte arrays.  The old layout (a
dict of lists of Python tuples) made every forked worker copy its share of the
index pages just by reading them: reading a tuple bumps its refcount and
triggers a copy-on-write fault, so on a whole-PBF z7-16 run the 13 workers piled
up past the 15 GB of the machine and the OOM killer ended the job.  Arrays are
never written while rendering, so the workers share them with no page churn.
The pipeline also builds one compact index per zoom and drops it right after
that zoom is rendered, and releases the vector cache before forking.

The container is not byte-reproducible: the payloads are laid out in the order
the workers finish, which varies between runs.  The tiles themselves are.

The rendering engine is designed to be bit-identical to the legacy one; the
legacy implementation is kept as ``render_toner_tile_reference`` and used by the
test-suite as an oracle.
"""

import os
import sys
import math
import time
import gzip
import json
import glob
import struct
import zlib
import argparse
import multiprocessing as mp
from array import array

# Dependencies
try:
    import requests
    from PIL import Image, ImageDraw, ImageFont
    import osmium
except ImportError as err:
    print(f"[!] Error: missing library: {err}")
    print("    Install the prerequisites with: pip install requests pillow rich osmium")
    sys.exit(1)

# Optional Rich TUI styling
try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
    from rich.progress import Progress, SpinnerColumn, BarColumn, TextColumn, TimeRemainingColumn
    from rich.prompt import Prompt, Confirm
    HAS_RICH = True
    console = Console()
except ImportError:
    HAS_RICH = False
    console = None

# Packed map container (.mmap). Lives next to this script; when the file is
# imported from another directory the sibling path is added explicitly.
try:
    from mono_map import MapWriter, pack_tile_image, blank_kind
except ImportError:  # pragma: no cover - only when imported from elsewhere
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mono_map import MapWriter, pack_tile_image, blank_kind


# --------------------------------------------------------------------------- #
# Fonts for place names
# --------------------------------------------------------------------------- #
FONTS = {}


def get_font(size, bold=True):
    key = (size, bold)
    if key in FONTS:
        return FONTS[key]
    font_paths = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf" if bold else "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf"
    ]
    for p in font_paths:
        if os.path.exists(p):
            try:
                f = ImageFont.truetype(p, size)
                FONTS[key] = f
                return f
            except Exception:
                pass
    f = ImageFont.load_default()
    FONTS[key] = f
    return f


# --------------------------------------------------------------------------- #
# Projection helpers
# --------------------------------------------------------------------------- #
def deg2num(lat_deg, lon_deg, zoom):
    lat_rad = math.radians(lat_deg)
    n = 2.0 ** zoom
    xtile = int((lon_deg + 180.0) / 360.0 * n)
    ytile = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return xtile, ytile


def num2deg(xtile, ytile, zoom):
    n = 2.0 ** zoom
    lon_deg = xtile / n * 360.0 - 180.0
    lat_rad = math.atan(math.sinh(math.pi * (1 - 2 * ytile / n)))
    lat_deg = math.degrees(lat_rad)
    return lat_deg, lon_deg


def world_x(lat, lon, n):
    """Global web-mercator X in [0..n] units (tile units, not pixels)."""
    return (lon + 180.0) / 360.0 * n


def world_y(lat, lon, n):
    """Global web-mercator Y in [0..n] units (tile units, not pixels)."""
    return (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n


def project_pt(lat, lon, tx, ty, n):
    """Projects geographic coordinate to local tile pixel (0..255).

    Kept byte-for-byte compatible with the legacy implementation.
    """
    px = (world_x(lat, lon, n) - tx) * 256.0
    py = (world_y(lat, lon, n) - ty) * 256.0
    return int(round(px)), int(round(py))


def tile_bounds(z, tx, ty):
    n = 2.0 ** z
    lon_min = tx / n * 360.0 - 180.0
    lon_max = (tx + 1) / n * 360.0 - 180.0
    lat_max = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * ty / n))))
    lat_min = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * (ty + 1) / n))))
    return lat_min, lon_min, lat_max, lon_max


# --------------------------------------------------------------------------- #
# Known Italian city coordinates for offline fallback
# --------------------------------------------------------------------------- #
KNOWN_CITIES = {
    "milano": (45.4642, 9.1900),
    "como": (45.8081, 9.0852),
    "varese": (45.8184, 8.8239),
    "lecco": (45.8559, 9.3902),
    "monza": (45.5845, 9.2744),
    "bergamo": (45.6983, 9.6773),
    "brescia": (45.5416, 10.2118),
    "sondrio": (46.1712, 9.8728),
    "pavia": (45.1847, 9.1582),
    "cremona": (45.1332, 10.0275),
    "mantova": (45.1564, 10.7914),
    "lodi": (45.3142, 9.5033),
    "torino": (45.0703, 7.6869),
    "novara": (45.4469, 8.6219),
    "verbania": (45.9228, 8.5518),
    "genova": (44.4056, 8.9463),
    "bologna": (44.4949, 11.3426),
    "roma": (41.9028, 12.4964),
    "firenze": (43.7696, 11.2558),
    "venezia": (45.4408, 12.3155),
    "verona": (45.4384, 10.9916),
    "trento": (46.0748, 11.1217),
    "bolzano": (46.4983, 11.3548),
    "aosta": (45.7373, 7.3196),
}


def geocode_city(query):
    q_clean = query.strip().lower()
    if q_clean in KNOWN_CITIES:
        lat, lon = KNOWN_CITIES[q_clean]
        return {"name": query.capitalize(), "lat": lat, "lon": lon}

    # Try online Nominatim if available
    url = "https://nominatim.openstreetmap.org/search"
    headers = {"User-Agent": "MonoMesh-TileDownloader/1.2 (Hardware: M5Stack Paper Mono)"}
    params = {"q": query, "format": "json", "limit": 1}
    try:
        resp = requests.get(url, headers=headers, params=params, timeout=5)
        if resp.status_code == 200:
            data = resp.json()
            if data:
                return {
                    "name": data[0].get("display_name", query),
                    "lat": float(data[0]["lat"]),
                    "lon": float(data[0]["lon"])
                }
    except Exception:
        pass
    return None


def find_pbf_files():
    """Finds available .osm.pbf files in current directory or workspace."""
    candidates = glob.glob("*.osm.pbf") + glob.glob("../*.osm.pbf")
    seen = set()
    result = []
    for c in candidates:
        abs_p = os.path.abspath(c)
        if abs_p not in seen and os.path.exists(abs_p):
            seen.add(abs_p)
            result.append(abs_p)
    return result


def pbf_header_bbox(pbf_path):
    """Bounding box declared in the PBF header (the whole file as an area).

    Reading the header costs one seek: no scan of the 500 MB file needed to show
    the plan before deciding whether the job is worth starting.
    """
    try:
        header = osmium.io.Reader(pbf_path).header()
        box = header.box()
        if not box.valid():
            return None
        bl, tr = box.bottom_left, box.top_right
        return (bl.lat, bl.lon, tr.lat, tr.lon)
    except Exception as err:
        print(f"[!] Cannot read the bounding box from {pbf_path}: {err}")
        return None


# Rough container size per tile (1 bit/pixel) measured on the real tiles, used
# only for the pre-flight estimate: the plan must be visible before the hours of
# rendering, not after.
TILE_SIZE_ESTIMATE = {8: 4200, 9: 3100, 10: 3600, 11: 2500, 12: 2700,
                      13: 3000, 14: 3000, 15: 1650, 16: 1000}


def tile_plan(bbox, zoom_levels, bits=1):
    """Tile counts per zoom plus a rough output size and time estimate."""
    per_zoom = {}
    for z in zoom_levels:
        per_zoom[z] = count_tiles(bbox, z)
    total = sum(per_zoom.values())
    est_bytes = sum(n * TILE_SIZE_ESTIMATE.get(z, 2500) for z, n in per_zoom.items())
    if bits == 2:
        est_bytes = int(est_bytes * 1.15)
    return {"per_zoom": per_zoom, "total": total, "bytes": est_bytes}


def print_tile_plan(plan, jobs, out_format, bits, whole_pbf=False, bbox=None):
    detail = out_format if out_format == "png" else \
        f"{out_format}, {1 if bits == 1 else 2} bit/pixel"
    print(f"[*] Plan: {plan['total']:,} tiles (format {detail})")
    for z, n in sorted(plan["per_zoom"].items()):
        print(f"      z{z:<2} {n:>9,} tiles")
    if out_format in ("mmap", "both"):
        print(f"    Estimated container: ~{plan['bytes'] / 1e6:.1f} MB")
    else:
        print("    PNG size: depends on the chosen quality (~2x the 1-bit container)")
    if plan["total"]:
        est_s = plan["total"] * 0.021 * 14.0 / max(1, jobs)
        print(f"    Estimated rendering: ~{est_s / 60:.1f} min with {jobs} processes "
              f"(excluding PBF extraction)")
    if whole_pbf:
        print(f"[!] Area = whole PBF file: {bbox[0]:.2f}..{bbox[2]:.2f} N, "
              f"{bbox[1]:.2f}..{bbox[3]:.2f} E "
              f"(~{(bbox[2] - bbox[0]) * 111:.0f}x{(bbox[3] - bbox[1]) * 82:.0f} km)")
        print("[!] Extracting the whole PBF takes a few minutes and several hundred MB "
              "of RAM: it is better to limit yourself to low zooms (e.g. 9,10,11,12) or generate one region at a time.")


# --------------------------------------------------------------------------- #
# Style tables (single source of truth, shared by fast and reference renderer)
# --------------------------------------------------------------------------- #
ROAD_ORDER = [
    "pedestrian", "path", "track", "service", "living_street",
    "residential", "unclassified", "tertiary", "tertiary_link",
    "secondary", "secondary_link", "primary", "primary_link",
    "trunk", "trunk_link", "motorway", "motorway_link",
]
ROAD_PRIO = {cls: idx for idx, cls in enumerate(ROAD_ORDER)}
ROAD_PRIO_NONE = 255

TYPE_HIGHWAY = 0
TYPE_WATER = 1
TYPE_RAILWAY = 2

# Water sub-kinds. For water ways this code lives in the `prio` byte (highway
# ways use the ROAD_ORDER index there, so the two never clash).
WATER_STREAM = 0       # stream / ditch / drain        -> minor line
WATER_RIVER = 1        # river / canal                 -> major line
WATER_AREA = 2         # natural=water / riverbank     -> area (fill when closed)
WATER_RING_OUTER = 3   # outer ring of a water multipolygon relation
WATER_RING_INNER = 4   # inner ring (island) of a water multipolygon relation

PLACE_KINDS = ["city", "town", "village", "suburb", "hamlet", "neighbourhood"]
PLACE_KIND_IDX = {name: idx for idx, name in enumerate(PLACE_KINDS)}

# Fallback importance of a label when the OSM `population` tag is missing.
PLACE_DEFAULT_SCORE = {0: 100000, 1: 30000, 2: 5000, 3: 3000, 4: 1000, 5: 1000}

# Roads shown from a given zoom on (the reference Toner style is deliberately
# sparse at low zoom: drawing every service road at z9 turns the tile into ink).
_ZOOM_ROADS = [
    (0, {"motorway", "trunk", "motorway_link", "trunk_link"}),
    (8, {"primary", "primary_link"}),
    (10, {"secondary", "secondary_link"}),
    (12, {"tertiary", "tertiary_link", "unclassified"}),
    (13, {"residential", "living_street"}),
    (14, {"service", "track", "pedestrian"}),
    (15, {"path"}),
]

# Labels shown from a given zoom on, and how many may share one tile.
_ZOOM_LABELS = [
    (0, {0}),            # city
    (10, {1}),           # town
    (13, {2, 3}),        # village, suburb
    (14, {4, 5}),        # hamlet, neighbourhood
]
_LABEL_BUDGET = [(7, 3), (9, 6), (11, 9), (13, 14), (99, 20)]


def zoom_road_classes(z):
    out = set()
    for zmin, classes in _ZOOM_ROADS:
        if z >= zmin:
            out |= classes
    return out


def zoom_label_kinds(z):
    out = set()
    for zmin, kinds in _ZOOM_LABELS:
        if z >= zmin:
            out |= kinds
    return out


def label_budget(z):
    for zmax, n in _LABEL_BUDGET:
        if z <= zmax:
            return n
    return 20


def water_min_size(z):
    """Minimum projected size (px) for a water area to be worth drawing."""
    if z <= 9:
        return 5.0
    if z <= 11:
        return 3.0
    if z <= 12:
        return 2.0
    return 0.0


# Rendering a tile walks every point of every way registered in it, and the long
# ones (rivers, motorways, coastlines) are registered in every tile they cross:
# on the whole north-west PBF at z15 the 2% of the items with more than 1024
# points account for 86% of the per-tile point visits.  Their bounding boxes are
# therefore also recorded in blocks of _CLIP_BLOCK points, so the renderer can
# find the slice of the way a tile can actually see instead of converting all of
# it.  The margin covers the half width of the widest road (3 px), its curve
# joints and the coordinate rounding.
_CLIP_BLOCK = 32
_CLIP_MIN_POINTS = 256
_CLIP_MARGIN = 16.0 / 256.0


def road_style(cls, z):
    """Returns (priority, line_width, has_white_casing) or None when not drawn."""
    prio = ROAD_PRIO.get(cls)
    if prio is None or cls not in zoom_road_classes(z):
        return None
    if cls in ("residential", "unclassified"):
        width = 2 if z >= 14 else 1
    elif cls in ("tertiary", "tertiary_link", "secondary_link"):
        width = 2
    elif cls == "secondary":
        width = 3 if z >= 13 else 2
    elif cls == "primary":
        width = 4 if z >= 13 else 3
    elif cls in ("primary_link", "trunk_link", "motorway_link"):
        width = 3
    elif cls == "trunk":
        width = 5 if z >= 13 else 4
    elif cls == "motorway":
        width = 6 if z >= 13 else 4
    else:
        width = 1
    # low zoom: hairline roads, like the reference style
    if z <= 9:
        width = 2 if cls in ("motorway", "trunk", "motorway_link", "trunk_link") else 1
    elif z <= 12:
        width = min(width, 2)
    casing = cls in ("trunk", "motorway") and z >= 10 and width >= 3
    return prio, width, casing


def water_line_width(z):
    return 3 if z >= 13 else 2


# --------------------------------------------------------------------------- #
# Compact vector container + binary cache
# --------------------------------------------------------------------------- #
CACHE_MAGIC = b"MOSMTILE1\x00"
# 3: water rings carry a meaningful "closed" flag (fillable vs only outlineable),
#    so caches written before this change must be regenerated.
CACHE_VERSION = 3
E5 = 100000.0  # 5 decimal places, same precision as round(lat, 5)


class VectorData:
    """Compact, array-backed container for the extracted vectors.

    Coordinates are stored as int32 micro-degrees (1e-5 deg), which is exactly
    the precision used by the legacy pipeline (``round(lat, 5)``) and keeps the
    cache small (8 bytes per point instead of ~120 bytes for Python tuples).
    """

    __slots__ = ("bbox", "types", "prio", "closed", "npts", "lat_e5", "lon_e5",
                 "place_lat_e5", "place_lon_e5", "place_kind", "place_population",
                 "place_names", "n_ways", "n_places", "n_points")

    def __init__(self, bbox):
        self.bbox = list(bbox)
        self.types = bytearray()
        self.prio = bytearray()
        self.closed = bytearray()
        self.npts = array("i")
        self.lat_e5 = array("i")
        self.lon_e5 = array("i")
        self.place_lat_e5 = array("i")
        self.place_lon_e5 = array("i")
        self.place_kind = bytearray()
        self.place_population = array("i")
        self.place_names = []
        self.n_ways = 0
        self.n_places = 0
        self.n_points = 0

    # -- building ---------------------------------------------------------- #
    def add_way(self, type_code, prio, closed, lats_e5, lons_e5):
        self.types.append(type_code)
        self.prio.append(prio)
        self.closed.append(1 if closed else 0)
        self.npts.append(len(lats_e5))
        self.lat_e5.extend(lats_e5)
        self.lon_e5.extend(lons_e5)
        self.n_ways += 1
        self.n_points += len(lats_e5)

    def add_place(self, lat, lon, kind, name, population=0):
        self.place_lat_e5.append(int(round(lat * E5)))
        self.place_lon_e5.append(int(round(lon * E5)))
        self.place_kind.append(PLACE_KIND_IDX.get(kind, 255))
        self.place_population.append(int(population) if population else 0)
        self.place_names.append(name)
        self.n_places += 1

    # -- convenience ------------------------------------------------------- #
    def way_class(self, idx):
        prio = self.prio[idx]
        if self.types[idx] != TYPE_HIGHWAY or prio >= len(ROAD_ORDER):
            return None
        return ROAD_ORDER[prio]

    def point_offset(self, idx):
        """O(n) only in the worst case; callers iterate sequentially."""
        return sum(self.npts[:idx])

    def iter_ways(self):
        off = 0
        for i in range(self.n_ways):
            cnt = self.npts[i]
            yield i, self.types[i], self.prio[i], self.closed[i], off, cnt
            off += cnt

    def to_legacy_dict(self):
        """Materialise the legacy list-of-dicts structure (used by tests)."""
        ways = []
        for i, type_code, prio, closed, off, cnt in self.iter_ways():
            if type_code == TYPE_HIGHWAY:
                cls = ROAD_ORDER[prio] if prio < len(ROAD_ORDER) else ""
                wtype = "highway"
                water_kind = None
            elif type_code == TYPE_WATER:
                cls, wtype = "water", "water"
                water_kind = prio
            else:
                cls, wtype = "railway", "railway"
                water_kind = None
            pts = [(self.lat_e5[j] / E5, self.lon_e5[j] / E5) for j in range(off, off + cnt)]
            entry = {"class": cls, "type": wtype, "closed": bool(closed), "name": "", "pts": pts}
            if water_kind is not None:
                entry["water_kind"] = water_kind
            ways.append(entry)
        places = []
        for i in range(self.n_places):
            kind = PLACE_KINDS[self.place_kind[i]] if self.place_kind[i] < len(PLACE_KINDS) else "other"
            places.append({
                "name": self.place_names[i],
                "lat": self.place_lat_e5[i] / E5,
                "lon": self.place_lon_e5[i] / E5,
                "type": kind,
                "population": self.place_population[i],
            })
        return {"bbox": list(self.bbox), "ways": ways, "places": places}

    @classmethod
    def from_legacy_dict(cls, data):
        obj = cls(data["bbox"])
        for w in data["ways"]:
            wtype = w["type"]
            if wtype == "highway":
                type_code = TYPE_HIGHWAY
                prio = ROAD_PRIO.get(w["class"], ROAD_PRIO_NONE)
            elif wtype == "water":
                type_code = TYPE_WATER
                prio = w.get("water_kind", WATER_AREA)
            else:
                type_code = TYPE_RAILWAY
                prio = ROAD_PRIO_NONE
            lats = array("i", (int(round(lat * E5)) for lat, _ in w["pts"]))
            lons = array("i", (int(round(lon * E5)) for _, lon in w["pts"]))
            obj.add_way(type_code, prio, w.get("closed", False), lats, lons)
        for p in data["places"]:
            obj.add_place(p["lat"], p["lon"], p.get("type", "other"), p.get("name", ""),
                          p.get("population", 0))
        return obj


def _pack_names(names):
    blob = bytearray()
    offs = array("i")
    lens = array("H")
    for name in names:
        raw = name.encode("utf-8", "replace")
        if len(raw) > 65535:
            raw = raw[:65535]
        offs.append(len(blob))
        lens.append(len(raw))
        blob.extend(raw)
    return bytes(blob), offs, lens


def cache_write(path, data):
    """Writes the compact binary cache (shared with the native C implementation)."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    blob, place_off, place_len = _pack_names(data.place_names)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(CACHE_MAGIC)
        f.write(struct.pack("<II", CACHE_VERSION, 0))
        f.write(struct.pack("<4d", *data.bbox))
        f.write(struct.pack("<III", data.n_ways, data.n_places, len(blob)))
        f.write(bytes(data.types))
        f.write(bytes(data.prio))
        f.write(bytes(data.closed))
        f.write(data.npts.tobytes())
        f.write(data.lat_e5.tobytes())
        f.write(data.lon_e5.tobytes())
        f.write(data.place_lat_e5.tobytes())
        f.write(data.place_lon_e5.tobytes())
        f.write(bytes(data.place_kind))
        f.write(data.place_population.tobytes())   # cache v2
        f.write(place_off.tobytes())
        f.write(place_len.tobytes())
        f.write(blob)
    os.replace(tmp, path)


def _read_array(f, typecode, count):
    """Reads ``count`` native-endian items straight from the cache file.

    Reading directly into the array avoids materialising the whole file in one
    ``bytes`` object first: the north-west cache is 199 MB and the transient copy
    was the largest spike of the load (the arrays are copied out of it anyway).
    """
    arr = array(typecode)
    if count:
        arr.fromfile(f, count)
    return arr


def cache_read(path):
    """Reads the compact binary cache produced by any MonoMesh tool."""
    with open(path, "rb") as f:
        if f.read(len(CACHE_MAGIC)) != CACHE_MAGIC:
            raise ValueError("not a MonoMesh binary cache")
        version, _flags = struct.unpack("<II", f.read(8))
        if version not in (1, CACHE_VERSION):
            raise ValueError(f"unsupported cache version {version}")
        bbox = struct.unpack("<4d", f.read(32))
        n_ways, n_places, blob_len = struct.unpack("<III", f.read(12))

        data = VectorData(bbox)
        data.n_ways = n_ways
        data.n_places = n_places
        data.types = bytearray(f.read(n_ways))
        data.prio = bytearray(f.read(n_ways))
        data.closed = bytearray(f.read(n_ways))
        data.npts = _read_array(f, "i", n_ways)
        n_points = sum(data.npts)
        data.n_points = n_points
        data.lat_e5 = _read_array(f, "i", n_points)
        data.lon_e5 = _read_array(f, "i", n_points)
        data.place_lat_e5 = _read_array(f, "i", n_places)
        data.place_lon_e5 = _read_array(f, "i", n_places)
        data.place_kind = bytearray(f.read(n_places))
        if version >= 2:
            data.place_population = _read_array(f, "i", n_places)
        else:
            data.place_population = array("i", bytes(4 * n_places))
        place_off = _read_array(f, "i", n_places)
        place_len = _read_array(f, "H", n_places)
        blob = f.read(blob_len)
    data.place_names = [blob[place_off[i]:place_off[i] + place_len[i]].decode("utf-8", "replace")
                        for i in range(n_places)]
    if sys.byteorder != "little":  # pragma: no cover - little-endian everywhere we ship
        for arr in (data.npts, data.lat_e5, data.lon_e5, data.place_lat_e5, data.place_lon_e5):
            arr.byteswap()
        data.place_population.byteswap()
    return data


def cache_version(path):
    """Returns the format version stored in a cache file (0 when unreadable)."""
    try:
        with open(path, "rb") as f:
            head = f.read(len(CACHE_MAGIC) + 8)
        if head[:len(CACHE_MAGIC)] != CACHE_MAGIC:
            return 0
        return struct.unpack_from("<I", head, len(CACHE_MAGIC))[0]
    except Exception:
        return 0


def cache_load_legacy_json(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        payload = json.load(f)
    return VectorData.from_legacy_dict(payload)


# --------------------------------------------------------------------------- #
# Extraction from PBF (optimised)
# --------------------------------------------------------------------------- #
# Classes that the Toner style never draws: skipped at extraction time so that
# neither the Python loop nor the cache has to carry them around.
UNRENDERED_HIGHWAY = {
    "footway", "cycleway", "steps", "bridleway", "construction", "proposed",
    "platform", "corridor", "elevator", "escalator", "bus_stop", "crossing",
    "traffic_island", "raceway", "busway", "rest_area", "services",
}

# waterway values drawn as minor lines (only at high zoom)
MINOR_WATERWAYS = {"stream", "ditch", "drain", "brook", "wadi", "tidal_channel",
                   "drainage_channel", "gutter", "ditch"}
# waterway values drawn as major lines
MAJOR_WATERWAYS = {"river", "canal", "fairway"}


def _is_water_relation(tags):
    return (tags.get("natural") in ("water", "wetland", "bay")
            or "water" in tags
            or tags.get("waterway") in ("riverbank", "river", "canal")
            or tags.get("landuse") in ("reservoir", "basin"))


def _parse_population(value):
    """Parses the OSM population tag ('12 345', '12345', '12,345')."""
    if not value:
        return 0
    digits = "".join(ch for ch in value if ch.isdigit())
    if not digits:
        return 0
    try:
        return int(digits)
    except ValueError:
        return 0


# struct format per point count, so decoding a WKB line string does not rebuild
# the same format string for every one of the ~2M ways.
_WKB_FMT_CACHE = {}


def _collect_water_relation_members(pbf_path, verbose=False):
    """Prepass over the relations: relation id -> [(way id, role)].

    Larger lakes and rivers in OSM are multipolygon relations whose outline is made
    of many untagged ways: without this step they are invisible on the map.

    The members stay grouped per relation on purpose. With a single flat way->role
    map the ring assembly chained ways of *different* relations whenever they shared
    a node, so Lake Como ended up attached to the Adda river: the ring never closed
    and filling it painted a straight black band across the map.
    """
    per_relation = {}
    t0 = time.time()
    kf = osmium.filter.KeyFilter("type", "natural", "water", "waterway", "landuse")
    fp = osmium.FileProcessor(pbf_path, osmium.osm.RELATION).with_filter(kf)
    n_rel = 0
    n_ways = 0
    for obj in fp:
        tags = obj.tags
        if tags.get("type") != "multipolygon" or not _is_water_relation(tags):
            continue
        n_rel += 1
        members = []
        for m in obj.members:
            if m.type != "w":
                continue
            # 0 = outer, 1 = inner
            members.append((m.ref, 1 if m.role == "inner" else 0))
        if members:
            per_relation[obj.id] = members
            n_ways += len(members)
    if verbose and n_rel:
        print(f"[*] Water relations (multipolygon): {n_rel} with {n_ways} member ways "
              f"(prepass {time.time() - t0:.1f}s)")
    return per_relation


def _segment_hits_rect(lat0, lon0, lat1, lon1, bbox):
    """True when the straight segment enters the extraction rectangle.

    Used on water rings: when a relation continues outside the extracted area the
    chain cannot close, and the rasteriser fills the ring by joining the two loose
    ends with a straight edge. If that edge stays outside the rectangle it is never
    rendered, so the ring can be filled safely (a lake cut by the map border); if it
    crosses the area it would paint a band across the map, and the ring is only
    outlined. Liang-Barsky clipping test, no divisions by zero.
    """
    min_lat, min_lon, max_lat, max_lon = bbox
    dlat = lat1 - lat0
    dlon = lon1 - lon0
    t0, t1 = 0.0, 1.0
    for delta, start, lo, hi in ((dlat, lat0, min_lat, max_lat),
                                 (dlon, lon0, min_lon, max_lon)):
        if delta == 0.0:
            if start < lo or start > hi:
                return False          # parallel and outside this slab
            continue
        ta = (lo - start) / delta
        tb = (hi - start) / delta
        if ta > tb:
            ta, tb = tb, ta
        if ta > t0:
            t0 = ta
        if tb < t1:
            t1 = tb
        if t0 > t1:
            return False
    return True


def _ring_can_be_filled(ring_lat, ring_lon, bbox):
    """Is this water ring safe to fill?

    A ring that closes on itself always is. A truncated one (the relation continues
    outside the extracted area) is safe only when the straight edge that the
    rasteriser would add stays outside `bbox` - that is the case of a lake cut by
    the map border. Riverbank relations crossing the whole area would produce a
    black band, so they are left open and drawn as an outline.
    """
    if not ring_lat:
        return False
    if ring_lat[0] == ring_lat[-1] and ring_lon[0] == ring_lon[-1]:
        return True
    return not _segment_hits_rect(ring_lat[-1], ring_lon[-1], ring_lat[0], ring_lon[0], bbox)


def _assemble_rings(way_records, refs, lats, lons):
    """Chains relation member ways into rings (see _collect_member_ways)."""
    """Chains relation member ways into rings.

    ``way_records`` is a list of ``(ref_off, ref_cnt, role, has_inside)`` where
    ``refs`` are OSM node ids.  Returns a list of ``(role, lats, lons)``.

    The walk extends in *both* directions from the first way. A lake outline is a
    loop, and the member list can be entered anywhere: walking one way only, the
    chain stops at a dead end while the other half of the loop is later picked up
    as a separate ring that can never close (that is how Lake Como ended up as an
    outline instead of a filled lake, and how riverbank relations turned into long
    straight black bands when they were filled anyway).
    """
    # endpoint node id -> list of (way index, end)
    endpoint_map = {}
    for wi, (off, cnt, _role, has_inside) in enumerate(way_records):
        if cnt < 2 or not has_inside:
            continue
        endpoint_map.setdefault(refs[off], []).append((wi, 0))
        endpoint_map.setdefault(refs[off + cnt - 1], []).append((wi, 1))

    used = bytearray(len(way_records))
    rings = []

    def unit_dir(lat0, lon0, lat1, lon1):
        """Local direction (lon scaled by cos(lat)) as a unit vector."""
        dlat = lat1 - lat0
        dlon = (lon1 - lon0) * math.cos(math.radians((lat0 + lat1) * 0.5))
        norm = math.hypot(dlat, dlon)
        return (dlat / norm, dlon / norm) if norm > 0 else (0.0, 0.0)

    def take(node, in_dir):
        """Unused member way that continues most smoothly through `node`.

        Picking the first candidate blindly is what made the chain leave the lake at
        a junction and follow a tributary: with a lake, a canal and a river sharing a
        node, the ring wandered off and could never be closed (or, worse, was closed
        with a straight edge across the map). The smallest turn wins.
        """
        best = None
        best_dot = -2.0
        for wi, end in endpoint_map.get(node, ()):
            if used[wi]:
                continue
            woff, wcnt, _r, _i = way_records[wi]
            if end == 0:
                out_dir = unit_dir(lats[woff], lons[woff], lats[woff + 1], lons[woff + 1])
            else:
                out_dir = unit_dir(lats[woff + wcnt - 1], lons[woff + wcnt - 1],
                                   lats[woff + wcnt - 2], lons[woff + wcnt - 2])
            dot = out_dir[0] * in_dir[0] + out_dir[1] * in_dir[1]
            if dot > best_dot:
                best_dot = dot
                best = (wi, end)
                if dot > 0.999:      # perfect continuation, nothing can beat it
                    break
        return best

    for start in range(len(way_records)):
        off, cnt, role, has_inside = way_records[start]
        if used[start] or cnt < 2 or not has_inside:
            continue
        used[start] = 1
        ring_lat = [lats[off + k] for k in range(cnt)]
        ring_lon = [lons[off + k] for k in range(cnt)]
        ring_role = role
        head = refs[off]
        tail = refs[off + cnt - 1]

        # forward: append at the tail
        while tail != head:
            nxt = take(tail, unit_dir(ring_lat[-2], ring_lon[-2], ring_lat[-1], ring_lon[-1]))
            if nxt is None:
                break
            wi, end = nxt
            used[wi] = 1
            woff, wcnt, wrole, _inside = way_records[wi]
            if wrole == 1:
                ring_role = 1
            if end == 0:  # forward
                for k in range(1, wcnt):
                    ring_lat.append(lats[woff + k])
                    ring_lon.append(lons[woff + k])
                tail = refs[woff + wcnt - 1]
            else:         # reversed
                for k in range(wcnt - 2, -1, -1):
                    ring_lat.append(lats[woff + k])
                    ring_lon.append(lons[woff + k])
                tail = refs[woff]

        # backward: prepend at the head, closing the other half of the loop
        while tail != head:
            # the ring continues from head towards its second point: a candidate
            # fits when its own direction through head is the opposite one
            nxt = take(head, unit_dir(ring_lat[1], ring_lon[1], ring_lat[0], ring_lon[0]))
            if nxt is None:
                break
            wi, end = nxt
            used[wi] = 1
            woff, wcnt, wrole, _inside = way_records[wi]
            if wrole == 1:
                ring_role = 1
            if end == 1:  # the way ends on the head: its points come first, in order
                front_lat = [lats[woff + k] for k in range(wcnt - 1)]
                front_lon = [lons[woff + k] for k in range(wcnt - 1)]
                head = refs[woff]
            else:         # the way starts on the head: prepend it reversed
                front_lat = [lats[woff + k] for k in range(wcnt - 1, 0, -1)]
                front_lon = [lons[woff + k] for k in range(wcnt - 1, 0, -1)]
                head = refs[woff + wcnt - 1]
            ring_lat = front_lat + ring_lat
            ring_lon = front_lon + ring_lon

        rings.append((ring_role, ring_lat, ring_lon))
    return rings


def _collect_member_ways(pbf_path, member_roles, bbox, verbose=False):
    """Collects the geometry of the ways that make up water relations.

    The outline ways of a lake carry no tags of their own (the relation does),
    so they never match the tag filter used for the main pass: they need their
    own pass driven by way id.
    """
    way_records = []
    index = {}
    refs = array("q")
    lats = array("i")
    lons = array("i")
    if not member_roles:
        return way_records, refs, lats, lons, index
    t0 = time.time()
    ids = sorted({wid for members in member_roles.values() for wid, _role in members})
    # NB: entities must stay unrestricted here, otherwise the location cache
    # used to build way geometries would not be filled.
    fp = osmium.FileProcessor(pbf_path).with_locations().with_filter(osmium.filter.IdFilter(ids))
    for obj in fp:
        if not obj.is_way():
            continue
        has_in = False
        for n in obj.nodes:
            loc = n.location
            if loc.valid() and bbox[0] <= loc.lat <= bbox[2] and bbox[1] <= loc.lon <= bbox[3]:
                has_in = True
                break
        if not has_in:
            continue
        start = len(refs)
        cnt = 0
        for n in obj.nodes:
            loc = n.location
            if loc.valid():
                refs.append(n.ref)
                lats.append(int(round(loc.lat * E5)))
                lons.append(int(round(loc.lon * E5)))
                cnt += 1
        if cnt >= 2:
            index[obj.id] = len(way_records)
            way_records.append((start, cnt, True))
    if verbose:
        print(f"[*] Relation member ways collected: {len(way_records)} "
              f"(id pass: {time.time() - t0:.1f}s)")
    return way_records, refs, lats, lons, index


def extract_area_from_pbf(pbf_path, min_lat, min_lon, max_lat, max_lon,
                         cache_path=None, verbose=True):
    """
    Extracts roads, waterways, water polygons and place names within BBox from PBF.

    Optimisations vs. the legacy implementation:
    - bbox membership is tested *before* building the geometry, so the ~85% of
      candidate ways that fall outside the area never allocate point lists;
    - highway classes that the style never draws are dropped in Python before
      any geometry work (they are not even written to the cache);
    - coordinates are collected straight into int32 arrays instead of tuples.
    """
    t0 = time.time()

    # Expand BBox slightly (15%) to ensure road segments connecting over borders are rendered smoothly
    dlat = (max_lat - min_lat) * 0.15
    dlon = (max_lon - min_lon) * 0.15
    b_min_lat = min_lat - dlat
    b_max_lat = max_lat + dlat
    b_min_lon = min_lon - dlon
    b_max_lon = max_lon + dlon

    if verbose:
        size_mb = os.path.getsize(pbf_path) / (1024 * 1024)
        print(f"[*] Scanning PBF file: {os.path.basename(pbf_path)} ({size_mb:.0f} MB)")
        print(f"[*] Extraction BBox: Lat [{b_min_lat:.4f}..{b_max_lat:.4f}], Lon [{b_min_lon:.4f}..{b_max_lon:.4f}]")

    data = VectorData((min_lat, min_lon, max_lat, max_lon))
    member_roles = _collect_water_relation_members(pbf_path, verbose=verbose)
    member_way_records, member_refs, member_lats, member_lons, member_index = _collect_member_ways(
        pbf_path, member_roles,
        (b_min_lat, b_min_lon, b_max_lat, b_max_lon), verbose=verbose)

    kf = osmium.filter.KeyFilter("highway", "waterway", "railway", "natural", "place")
    fp = osmium.FileProcessor(pbf_path).with_locations().with_filter(kf)

    n_way_candidates = 0
    n_places = 0
    unrendered = UNRENDERED_HIGHWAY
    lats_buf = array("i")
    lons_buf = array("i")
    wkb = osmium.geom.WKBFactory()
    progress = None
    task = None
    scanned = 0

    def way_coords_in_bbox(obj):
        """E5 coordinates of a way, or (None, None) when it misses the rectangle.

        The geometry is taken from the C++ WKB factory: one call per way plus a
        single bulk unpack, instead of two Python loops that resolve the
        location of every node one by one (24M of them on the whole north-west
        PBF).  A way whose nodes cannot all be resolved - or that has fewer than
        two - makes the factory raise, and the node-by-node path below takes
        over, so the result is the same one the slow path produced.
        """
        try:
            raw = bytes.fromhex(wkb.create_linestring(obj))
            npts = struct.unpack_from("<I", raw, 5)[0]
        except Exception:
            npts = -1
        if npts >= 0:
            if npts < 2:
                return None, None
            fmt = _WKB_FMT_CACHE.get(npts)
            if fmt is None:
                fmt = _WKB_FMT_CACHE[npts] = "<%dd" % (2 * npts)
            vals = struct.unpack_from(fmt, raw, 9)
            # WKB stores (x, y) = (longitude, latitude), so the pairs are read
            # with the odd indices holding the latitude.
            has_in = False
            for k in range(1, 2 * npts, 2):
                if b_min_lat <= vals[k] <= b_max_lat and b_min_lon <= vals[k - 1] <= b_max_lon:
                    has_in = True
                    break
            if not has_in:
                return None, None
            return ([int(round(vals[k] * E5)) for k in range(1, 2 * npts, 2)],
                    [int(round(vals[k] * E5)) for k in range(0, 2 * npts, 2)])

        # Fallback: resolve the nodes one by one, skipping the invalid ones.
        has_in = False
        for n in obj.nodes:
            loc = n.location
            if loc.valid():
                lat = loc.lat
                lon = loc.lon
                if b_min_lat <= lat <= b_max_lat and b_min_lon <= lon <= b_max_lon:
                    has_in = True
                    break
        if not has_in:
            return None, None

        del lats_buf[:]
        del lons_buf[:]
        for n in obj.nodes:
            loc = n.location
            if loc.valid():
                lats_buf.append(int(round(loc.lat * E5)))
                lons_buf.append(int(round(loc.lon * E5)))
        return list(lats_buf), list(lons_buf)

    def handle(obj):
        nonlocal n_way_candidates, n_places
        if obj.is_node():
            tags = obj.tags
            if "place" in tags:
                loc = obj.location
                if loc.valid() and b_min_lat <= loc.lat <= b_max_lat and b_min_lon <= loc.lon <= b_max_lon:
                    data.add_place(loc.lat, loc.lon, tags["place"], tags.get("name", ""),
                                   _parse_population(tags.get("population")))
                    n_places += 1
            return
        if not obj.is_way():
            return

        tags = obj.tags
        hw = tags.get("highway")
        if hw is not None and hw in unrendered:
            return
        rw = tags.get("railway")
        ww = tags.get("waterway")
        nat = tags.get("natural")

        if not hw and not rw and not ww and nat != "water":
            return

        n_way_candidates += 1

        lats, lons = way_coords_in_bbox(obj)
        if lats is None:
            return
        if len(lats) < 2:
            return

        closed = len(lats) >= 4 and lats[0] == lats[-1] and lons[0] == lons[-1]
        if hw:
            type_code = TYPE_HIGHWAY
            prio = ROAD_PRIO.get(hw, ROAD_PRIO_NONE)
        elif rw:
            type_code = TYPE_RAILWAY
            prio = ROAD_PRIO_NONE
        else:
            type_code = TYPE_WATER
            if ww in MINOR_WATERWAYS:
                prio = WATER_STREAM
            elif ww in MAJOR_WATERWAYS:
                prio = WATER_RIVER
            elif ww is not None and ww != "riverbank":
                prio = WATER_STREAM
            else:
                prio = WATER_AREA
        data.add_way(type_code, prio, closed, lats, lons)

    if verbose and HAS_RICH and sys.stdout.isatty():
        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            TextColumn("[progress.percentage]{task.completed:,.0f} filtered entities"),
            TimeRemainingColumn(),
            console=console
        ) as progress_ctx:
            progress = progress_ctx
            task = progress.add_task("[cyan]Reading and extracting geometries from PBF...", total=None)
            for obj in fp:
                scanned += 1
                if scanned % 500000 == 0:
                    progress.advance(task, 500000)
                handle(obj)
    else:
        for obj in fp:
            scanned += 1
            if verbose and scanned % 5000000 == 0:
                print(f"[*] Scanned {scanned // 1000000}M objects...")
            handle(obj)

    # Lakes and wide rivers are multipolygon relations: their outline ways have
    # just been collected, now they get chained into closed rings - one relation at
    # a time, so ways of different relations (a lake and the river leaving it) are
    # never chained together just because they share a node.
    n_rings = 0
    if member_way_records:
        for rel_id in sorted(member_roles):
            records = []
            for way_id, role in member_roles[rel_id]:
                j = member_index.get(way_id)
                if j is None:
                    continue
                off, cnt, has_inside = member_way_records[j]
                records.append((off, cnt, role, has_inside))
            for role, ring_lat, ring_lon in _assemble_rings(records, member_refs,
                                                            member_lats, member_lons):
                if len(ring_lat) < 4:
                    continue
                data.add_way(TYPE_WATER, WATER_RING_INNER if role else WATER_RING_OUTER,
                             # the ring points are in e5 units, the bbox in degrees
                             _ring_can_be_filled([v / E5 for v in ring_lat],
                                                 [v / E5 for v in ring_lon],
                                                 (b_min_lat, b_min_lon, b_max_lat, b_max_lon)),
                             array("i", ring_lat), array("i", ring_lon))
                n_rings += 1
        if verbose and n_rings:
            print(f"    Water rings from relations: {n_rings} "
                  f"({sum(1 for i in range(data.n_ways) if data.prio[i] == WATER_RING_INNER)} inner)")

    elapsed = time.time() - t0
    if verbose:
        print(f"[✓] Extraction completed in {elapsed:.1f}s!")
        print(f"    Extracted elements: {data.n_ways} road/waterway segments, {data.n_places} places.")

    if cache_path:
        cache_write(cache_path, data)
        if verbose:
            print(f"[*] Cache saved to: {cache_path} ({os.path.getsize(cache_path) / 1024:.0f} KB)")

    return data


def cache_is_stale(cache_path, bbox):
    """True when an existing cache was built for a different bounding box."""
    try:
        with open(cache_path, "rb") as f:
            head = f.read(len(CACHE_MAGIC) + 8 + 32)
        if head[:len(CACHE_MAGIC)] != CACHE_MAGIC:
            return True
        cached = struct.unpack_from("<4d", head, len(CACHE_MAGIC) + 8)
    except Exception:
        return True
    return any(abs(a - b) > 1e-9 for a, b in zip(cached, bbox))


# --------------------------------------------------------------------------- #
# Tile index (per zoom) - the core of the speed-up
# --------------------------------------------------------------------------- #
# Draw codes of the compact index: how the renderer must paint a way.  They
# replace the old class strings, so the per-way style fits in byte arrays.
DRAW_ROAD = 0
DRAW_WATER_FILL = 1
DRAW_WATER_LINE = 2
DRAW_WATER_INNER_FILL = 3
DRAW_WATER_INNER_LINE = 4
DRAW_RAILWAY = 5
DRAW_NONE = 255


class TileIndex:
    """Per-zoom spatial index: every way is registered only in the tiles it touches.

    Buckets live in a compact CSR layout: one dense per-tile offset grid plus a
    flat int32 array of way ids, already in drawing order.  The old layout (a
    dict of lists of Python tuples) was the memory bomb of whole-PBF runs: every
    forked worker that merely *reads* a tuple bumps its refcount and the kernel
    copies the page, so on z7..16 the 11.9M registrations were multiplied by the
    number of workers.  Plain arrays are shared copy-on-write without page churn.
    """

    __slots__ = ("zoom", "xs", "ys", "scale", "x0", "y0", "nx", "ny",
                 "tile_offsets", "tile_items", "way_off", "way_code", "way_width",
                 "way_casing", "way_cls", "way_blk", "place_buckets",
                 "blk_min_x", "blk_max_x", "blk_min_y", "blk_max_y")

    def __init__(self, zoom, xs=None, ys=None, scale=1.0, x0=0, y0=0, nx=0, ny=0):
        self.zoom = zoom
        self.xs = xs
        self.ys = ys
        self.scale = scale
        self.x0 = x0
        self.y0 = y0
        self.nx = nx
        self.ny = ny
        self.tile_offsets = array("i")
        self.tile_items = array("i")
        self.way_off = None
        self.way_code = None
        self.way_width = None
        self.way_casing = None
        self.way_cls = None
        self.way_blk = None
        self.place_buckets = {}
        # Per-block bounding boxes of the long ways (see build_tile_index).
        self.blk_min_x = None
        self.blk_max_x = None
        self.blk_min_y = None
        self.blk_max_y = None

    def candidate_count(self):
        return len(self.tile_items)

    def tile_span(self, tx, ty):
        """Slice of :attr:`tile_items` registered in tile (tx, ty); (0, 0) if none."""
        if tx < self.x0 or ty < self.y0 or tx - self.x0 >= self.nx or ty - self.y0 >= self.ny:
            return 0, 0
        tid = (ty - self.y0) * self.nx + (tx - self.x0)
        return self.tile_offsets[tid], self.tile_offsets[tid + 1]

    @property
    def buckets(self):
        """Legacy ``{tile: [item tuples]}`` view (tests and debugging only).

        The renderer never uses it: rebuilding the tuples would put back exactly
        the Python objects this layout exists to avoid in forked workers.
        """
        out = {}
        for ty in range(self.y0, self.y0 + self.ny):
            for tx in range(self.x0, self.x0 + self.nx):
                start, end = self.tile_span(tx, ty)
                if start != end:
                    out[(tx, ty)] = [self.item_at(i) for i in self.tile_items[start:end]]
        return out

    def item_at(self, widx):
        """The legacy item tuple of way ``widx`` (see :attr:`buckets`)."""
        code = self.way_code[widx]
        off = self.way_off[widx]
        cnt = self.way_off[widx + 1] - off
        if code == DRAW_ROAD:
            cls = ROAD_ORDER[self.way_cls[widx]]
            prio = self.way_cls[widx]
        elif code in (DRAW_WATER_FILL, DRAW_WATER_LINE):
            cls, prio = "water", -3
        elif code in (DRAW_WATER_INNER_FILL, DRAW_WATER_INNER_LINE):
            cls, prio = "water_inner", -2
        else:
            cls, prio = "railway", -1
        return (prio, off, cnt, cls, self.way_width[widx],
                self.way_casing[widx], self.way_blk[widx])


def _way_offsets(data):
    """Prefix sums of the point counts: way id -> start of its points."""
    off = array("i", bytes(4 * (data.n_ways + 1)))
    acc = 0
    npts = data.npts
    for i in range(data.n_ways):
        off[i] = acc
        acc += npts[i]
    off[data.n_ways] = acc
    return off


def build_tile_index(data, z, locations=None, boxes=None, way_off=None):
    """Builds the compact tile buckets for one zoom level.

    ``locations`` may be a pre-computed (x_world, y_world) pair of float64 arrays
    at a reference zoom; they are rescaled exactly (power-of-two division) to
    the requested zoom, which avoids re-running the trigonometry per zoom and
    keeps the result bit-identical (scaling by a power of two is exact).

    ``boxes`` (from :func:`projected_way_boxes`) carries the per-way projected
    bounding boxes at that reference zoom; with them the tile range of a way is
    read straight from the box instead of walking its points again, which is
    what makes indexing many zoom levels in a row cheap.

    ``way_off`` may carry a precomputed :func:`_way_offsets` seed (it does not
    depend on the zoom, so the pipeline computes it once).

    The ways are registered into a dense grid of counts first, then filled in
    drawing order: a counting sort by class priority that walks the file order
    inside each class, exactly the order the old stable per-bucket sort gave.
    """
    n = 2.0 ** z
    if locations is None:
        locations = project_ways_at_zoom(data, z)
    ref_zoom, xs_all, ys_all = locations[0], locations[1], locations[2]
    if boxes is None and len(locations) > 3:
        boxes = locations[3]
    if ref_zoom == z:
        scale = 1.0
    else:
        scale = 2.0 ** (z - ref_zoom)

    if way_off is None:
        way_off = _way_offsets(data)

    # tile_range returns (x0, x1, y0, y1): turn it into origin + size of the
    # dense grid the CSR layout is based on.
    grid_x0, grid_x1, grid_y0, grid_y1 = tile_range(data.bbox, z)
    x0 = grid_x0
    y0 = grid_y0
    nx = grid_x1 - grid_x0 + 1
    ny = grid_y1 - grid_y0 + 1
    index = TileIndex(z, xs_all, ys_all, scale, x0, y0, nx, ny)
    index.way_off = way_off
    nways = data.n_ways
    types = data.types
    prio_arr = data.prio
    closed_arr = data.closed
    npts = data.npts

    way_code = index.way_code = bytearray(b"\xff" * nways)   # DRAW_NONE
    way_width = index.way_width = bytearray(nways)
    way_casing = index.way_casing = bytearray(nways)
    way_cls = index.way_cls = bytearray(prio_arr)            # original class byte
    # Drawing order key, shifted by +3 so it indexes a 20-slot counting-sort
    # table: water = -3 -> 0, inner rings = -2 -> 1, railways = -1 -> 2, roads =
    # ROAD_PRIO -> 3 + prio.  255 stays for "not drawn at this zoom".
    way_prio = bytearray(b"\xff" * nways)
    way_blk = index.way_blk = array("i", b"\xff\xff\xff\xff" * nways)  # all -1

    # Long ways get their per-block bounding boxes: the renderer uses them to
    # convert only the slice of the way a tile can see (see _CLIP_BLOCK).
    blk_min_x = array("d")
    blk_max_x = array("d")
    blk_min_y = array("d")
    blk_max_y = array("d")

    # Per-way tile range of the drawn ways: the CSR fill reuses it instead of
    # walking the geometry (or the bounding boxes) a second time.
    rng_x0 = array("i", bytes(4 * nways))
    rng_x1 = array("i", bytes(4 * nways))
    rng_y0 = array("i", bytes(4 * nways))
    rng_y1 = array("i", bytes(4 * nways))

    grid_x1 = x0 + nx - 1
    grid_y1 = y0 + ny - 1
    cells = nx * ny
    counts = array("i", bytes(4 * cells))

    off = 0
    for i in range(nways):
        cnt = npts[i]
        type_code = types[i]
        prio = prio_arr[i]
        if type_code == TYPE_HIGHWAY:
            if prio >= len(ROAD_ORDER):
                off += cnt  # class not drawn by the Toner style
                continue
            cls = ROAD_ORDER[prio]
            style = road_style(cls, z)
            if style is None:      # this class is not shown at this zoom
                off += cnt
                continue
            code = DRAW_ROAD
            width = style[1]
            casing = style[2]
            item_prio = style[0]
        elif type_code == TYPE_WATER:
            kind = prio
            # The `closed` flag of a ring means "safe to fill": the extraction sets it
            # for rings that close and for truncated ones whose invented closing edge
            # stays outside the extracted area (lakes cut by the map border). Rings
            # that are not safe to fill are outlined instead, otherwise the rasteriser
            # would join their loose ends with a straight edge and paint a band.
            ring_open = kind in (WATER_RING_OUTER, WATER_RING_INNER) and not closed_arr[i]
            if kind == WATER_RING_INNER:
                code = DRAW_WATER_INNER_LINE if ring_open else DRAW_WATER_INNER_FILL
                item_prio = -2
            elif kind == WATER_RING_OUTER:
                code = DRAW_WATER_LINE if ring_open else DRAW_WATER_FILL
                item_prio = -3
            elif kind == WATER_AREA:
                if z < 10:
                    off += cnt
                    continue
                code = DRAW_WATER_FILL if closed_arr[i] else DRAW_WATER_LINE
                item_prio = -3
            elif kind == WATER_RIVER:
                if z < 10:
                    off += cnt
                    continue
                code = DRAW_WATER_LINE
                item_prio = -3
            else:  # WATER_STREAM (ruscelli, fossi, drenaggi)
                if z < 13:
                    off += cnt
                    continue
                code = DRAW_WATER_LINE
                item_prio = -3
            width = 0 if code in (DRAW_WATER_FILL, DRAW_WATER_INNER_FILL) else water_line_width(z)
            casing = False
        else:
            code = DRAW_RAILWAY
            item_prio = -1
            width = 1
            casing = False

        # Tile range from the projected bounding box: no per-point work here.
        if boxes is not None:
            # The boxes were measured at the reference zoom in one single pass
            # for every zoom at once (see project_ways_at_zoom).  Rescaling by a
            # power of two is exact, so this is the same value the point loop
            # below would produce.
            bx, bx1, by, by1 = boxes
            min_x = bx[i]
            max_x = bx1[i]
            min_y = by[i]
            max_y = by1[i]
            if scale != 1.0:
                min_x *= scale
                max_x *= scale
                min_y *= scale
                max_y *= scale
        else:
            min_x = min_y = float("inf")
            max_x = max_y = float("-inf")
            for j in range(off, off + cnt):
                x = xs_all[j]
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                y = ys_all[j]
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y
            if scale != 1.0:
                min_x *= scale
                max_x *= scale
                min_y *= scale
                max_y *= scale

        # Tiny water bodies below the zoom's minimum size would only paint a
        # random looking single dot: drop them (the reference style does too).
        if type_code == TYPE_WATER:
            min_size = water_min_size(z)
            if min_size > 0.0:
                size_px = max(max_x - min_x, max_y - min_y) * 256.0
                if size_px < min_size:
                    off += cnt
                    continue

        # The renderer only ever asks for tiles inside the extracted bbox grid:
        # clamp the range here and skip ways that fall fully outside it.
        gx0 = int(math.floor(min_x))
        gx1 = int(math.floor(max_x))
        gy0 = int(math.floor(min_y))
        gy1 = int(math.floor(max_y))
        if gx0 < x0:
            gx0 = x0
        if gy0 < y0:
            gy0 = y0
        if gx1 > grid_x1:
            gx1 = grid_x1
        if gy1 > grid_y1:
            gy1 = grid_y1
        if gx1 < gx0 or gy1 < gy0:
            off += cnt
            continue

        # A long way is drawn as a line: record where its block boxes start so
        # the renderer can skip the points that are far from the tile.  Filled
        # water is a polygon and needs the whole ring, so it keeps blk == -1.
        blk = -1
        if width != 0 and cnt >= _CLIP_MIN_POINTS:
            blk = len(blk_min_x)
            end = off + cnt
            for b in range(off, end, _CLIP_BLOCK):
                # The block covers the segments *starting* at its points, so it
                # has to reach the first point of the next block: a segment
                # leaving the block is part of it.
                bend = b + _CLIP_BLOCK + 1
                if bend > end:
                    bend = end
                bx = bx1 = xs_all[b]
                by = by1 = ys_all[b]
                for j in range(b + 1, bend):
                    x = xs_all[j]
                    if x < bx:
                        bx = x
                    elif x > bx1:
                        bx1 = x
                    y = ys_all[j]
                    if y < by:
                        by = y
                    elif y > by1:
                        by1 = y
                blk_min_x.append(bx)
                blk_max_x.append(bx1)
                blk_min_y.append(by)
                blk_max_y.append(by1)

        way_code[i] = code
        way_width[i] = width
        way_casing[i] = 1 if casing else 0
        way_blk[i] = blk
        way_prio[i] = item_prio + 3
        rng_x0[i] = gx0
        rng_x1[i] = gx1
        rng_y0[i] = gy0
        rng_y1[i] = gy1
        for ty in range(gy0, gy1 + 1):
            row = (ty - y0) * nx
            for tx in range(gx0, gx1 + 1):
                counts[row + tx - x0] += 1
        off += cnt

    # Counting sort of the drawn ways by drawing priority; stable, so inside a
    # class the file order is preserved (same order as the old bucket sort).
    prio_count = [0] * 20
    for i in range(nways):
        p = way_prio[i]
        if p != DRAW_NONE:
            prio_count[p] += 1
    starts = [0] * 21
    acc = 0
    for p in range(20):
        starts[p] = acc
        acc += prio_count[p]
    starts[20] = acc
    order = array("i", bytes(4 * acc))
    cursor = starts[:]
    for i in range(nways):
        p = way_prio[i]
        if p != DRAW_NONE:
            order[cursor[p]] = i
            cursor[p] += 1

    # CSR offsets over the tile grid, then the fill pass.
    tile_offsets = index.tile_offsets = array("i", bytes(4 * (cells + 1)))
    acc = 0
    for c in range(cells):
        tile_offsets[c] = acc
        acc += counts[c]
    tile_offsets[cells] = acc
    tile_items = index.tile_items = array("i", bytes(4 * acc))
    cursor = list(tile_offsets[:cells])
    for i in order:
        gx0 = rng_x0[i]
        gx1 = rng_x1[i]
        gy0 = rng_y0[i]
        gy1 = rng_y1[i]
        for ty in range(gy0, gy1 + 1):
            row = (ty - y0) * nx
            for tx in range(gx0, gx1 + 1):
                tid = row + tx - x0
                tile_items[cursor[tid]] = i
                cursor[tid] += 1

    index.blk_min_x = blk_min_x
    index.blk_max_x = blk_max_x
    index.blk_min_y = blk_min_y
    index.blk_max_y = blk_max_y

    inv_e5 = 1.0 / E5
    for i in range(data.n_places):
        lat = data.place_lat_e5[i] * inv_e5
        lon = data.place_lon_e5[i] * inv_e5
        tx = int(math.floor(world_x(lat, lon, n)))
        ty = int(math.floor(world_y(lat, lon, n)))
        entry = (lat, lon, data.place_kind[i], data.place_names[i], data.place_population[i])
        index.place_buckets.setdefault((tx, ty), []).append(entry)

    return index


def projection_needs(zooms):
    """What :func:`project_ways_at_zoom` has to project for these zoom levels.

    Everything the style never draws at any of ``zooms`` can be skipped: unlike
    the extraction filters, which are about what belongs in the cache, this one
    is about what the renderer can possibly ask for.
    """
    road_ok = bytearray(len(ROAD_ORDER))
    for z in zooms:
        for cls in zoom_road_classes(z):
            road_ok[ROAD_PRIO[cls]] = 1
    # Water areas and rivers exist from z10 on, streams and ditches from z13;
    # water relation rings are always drawn.
    return road_ok, any(z >= 10 for z in zooms), any(z >= 13 for z in zooms)


def project_ways_at_zoom(data, z, zooms=None):
    """Pre-computes global mercator coordinates for the points that get drawn.

    Returns ``(z, xs, ys)`` with two float64 arrays indexed like the coordinate
    arrays; other zooms are derived by exact power-of-two scaling.

    Only the ways that at least one of ``zooms`` renders receive coordinates:
    for everything else the entries stay 0.0, which nobody ever reads because
    :func:`build_tile_index` drops the undrawn classes before touching the
    geometry.  A low-zoom run draws a small fraction of the ways, and the
    projection is the most expensive part of building the index.
    """
    n = 2.0 ** z
    if zooms is None:
        zooms = (z,)
    road_ok, water_area_ok, water_stream_ok = projection_needs(zooms)
    n_points = data.n_points
    # Zero-filled on purpose: the untouched entries are the undrawn ways.
    xs = array("d", bytes(8 * n_points))
    ys = array("d", bytes(8 * n_points))
    inv_e5 = 1.0 / E5
    lat_e5 = data.lat_e5
    lon_e5 = data.lon_e5
    asinh = math.asinh
    tan = math.tan
    radians = math.radians
    pi = math.pi

    for _i, type_code, prio, _closed, off, cnt in data.iter_ways():
        if not cnt:
            continue
        if not _drawn_by_zooms(type_code, prio, road_ok, water_area_ok, water_stream_ok):
            continue

        end = off + cnt
        la = lat_e5[off:end]
        lo = lon_e5[off:end]
        xs[off:end] = array("d", [(v * inv_e5 + 180.0) / 360.0 * n for v in lo])
        ys[off:end] = array("d", [(1.0 - asinh(tan(radians(v * inv_e5))) / pi) / 2.0 * n
                                  for v in la])

    return (z, xs, ys)


def _drawn_by_zooms(type_code, prio, road_ok, water_area_ok, water_stream_ok):
    """Can any of the projected zoom levels draw this way at all?"""
    if type_code == TYPE_HIGHWAY:
        return prio < len(road_ok) and road_ok[prio]
    if type_code == TYPE_WATER:
        if prio == WATER_STREAM:
            return water_stream_ok
        if prio in (WATER_RIVER, WATER_AREA):
            return water_area_ok
        return True                 # WATER_RING_OUTER / WATER_RING_INNER
    return True                     # TYPE_RAILWAY


def projected_way_boxes(data, locations, zooms=None):
    """Per-way projected bounding box, at the zoom ``locations`` was built for.

    Building the tile index for one zoom needs the box of every way it draws;
    doing that inside the per-zoom build walks the same points once per zoom
    level.  This walks them once for all of them.  Rescaling a box by a power of
    two is exact, so the tile ranges - and therefore the buckets - come out
    identical.

    Returns ``(min_x, max_x, min_y, max_y)``, four float64 arrays indexed by way.
    """
    ref_zoom, xs_all, ys_all = locations
    if zooms is None:
        zooms = (ref_zoom,)
    road_ok, water_area_ok, water_stream_ok = projection_needs(zooms)
    n_ways = data.n_ways
    min_x = array("d", bytes(8 * n_ways))
    max_x = array("d", bytes(8 * n_ways))
    min_y = array("d", bytes(8 * n_ways))
    max_y = array("d", bytes(8 * n_ways))
    for i, type_code, prio, _closed, off, cnt in data.iter_ways():
        if not cnt or not _drawn_by_zooms(type_code, prio, road_ok,
                                          water_area_ok, water_stream_ok):
            continue
        bx = bx1 = xs_all[off]
        by = by1 = ys_all[off]
        for j in range(off + 1, off + cnt):
            x = xs_all[j]
            if x < bx:
                bx = x
            elif x > bx1:
                bx1 = x
            y = ys_all[j]
            if y < by:
                by = y
            elif y > by1:
                by1 = y
        min_x[i] = bx
        max_x[i] = bx1
        min_y[i] = by
        max_y[i] = by1
    return (min_x, max_x, min_y, max_y)


# --------------------------------------------------------------------------- #
# Fast tile rendering
# --------------------------------------------------------------------------- #
def _visible_span(index, blk, off, cnt, tx, ty, scale):
    """Contiguous point range of a long way that can paint inside tile (tx, ty).

    The way's blocks of ``_CLIP_BLOCK`` points carry their bounding box at the
    reference zoom; the ones that miss the tile (plus the margin) are dropped.
    Every segment of a way lies inside the box of the block it belongs to, so a
    segment that could touch the tile always keeps its block: the pixels are the
    same ones the full polyline would paint, and the conversion is done only for
    the points that matter.  Returns ``(0, 0)`` when the way cannot be seen.
    """
    hi_x = tx + 1.0 + _CLIP_MARGIN
    lo_x = tx - _CLIP_MARGIN
    hi_y = ty + 1.0 + _CLIP_MARGIN
    lo_y = ty - _CLIP_MARGIN
    bminx = index.blk_min_x
    bmaxx = index.blk_max_x
    bminy = index.blk_min_y
    bmaxy = index.blk_max_y
    nblk = (cnt + _CLIP_BLOCK - 1) // _CLIP_BLOCK
    first = -1
    for k in range(nblk):
        b = blk + k
        if bminx[b] * scale <= hi_x and bmaxx[b] * scale >= lo_x and \
           bminy[b] * scale <= hi_y and bmaxy[b] * scale >= lo_y:
            first = k
            break
    if first < 0:
        return 0, 0
    last = first
    for k in range(nblk - 1, first, -1):
        b = blk + k
        if bminx[b] * scale <= hi_x and bmaxx[b] * scale >= lo_x and \
           bminy[b] * scale <= hi_y and bmaxy[b] * scale >= lo_y:
            last = k
            break
    j0 = off + first * _CLIP_BLOCK
    # One point past the last selected block: that point closes the last
    # segment of the block, which is part of it (see build_tile_index).
    j1 = off + (last + 1) * _CLIP_BLOCK + 1
    if j1 > off + cnt:
        j1 = off + cnt
    if j1 - j0 < 2:          # a single point paints nothing
        return 0, 0
    return j0, j1


def render_tile_image(z, tx, ty, index, style_dark=False):
    """Renders one 256x256 Toner tile and returns the PIL image (mode "L").

    Keeping the raster and the encoding separate is what lets the same renderer
    feed either PNG files or the packed .mmap container: the container writer
    works on this image, so a different style only changes the pixels.
    """
    bg_color = 0 if style_dark else 255
    fg_color = 255 if style_dark else 0
    im = Image.new("L", (256, 256), bg_color)
    draw = ImageDraw.Draw(im)

    start, end = index.tile_span(tx, ty)
    if start != end:
        xs_all = index.xs
        ys_all = index.ys
        scale = index.scale
        items = index.tile_items
        way_code = index.way_code
        way_width = index.way_width
        way_casing = index.way_casing
        way_blk = index.way_blk
        way_off = index.way_off
        # Flat [x0, y0, x1, y1, ...]: Pillow accepts it directly, and it costs
        # one int per coordinate instead of a tuple per point.
        px_pts = []
        append = px_pts.append
        for k in range(start, end):
            widx = items[k]
            off = way_off[widx]
            cnt = way_off[widx + 1] - off
            blk = way_blk[widx]
            if blk >= 0:
                j0, j1 = _visible_span(index, blk, off, cnt, tx, ty, scale)
                if j0 == j1:
                    continue
            else:
                j0 = off
                j1 = off + cnt
            del px_pts[:]
            if scale == 1.0:
                for j in range(j0, j1):
                    append(round((xs_all[j] - tx) * 256.0))
                    append(round((ys_all[j] - ty) * 256.0))
            else:
                for j in range(j0, j1):
                    append(round((xs_all[j] * scale - tx) * 256.0))
                    append(round((ys_all[j] * scale - ty) * 256.0))
            _draw_way(draw, z, way_code[widx], way_width[widx],
                      way_casing[widx], px_pts, fg_color, bg_color)

    draw_labels(draw, z, tx, ty, index.place_buckets.get((tx, ty), ()), fg_color, bg_color)
    return im


def render_tile_fast(z, tx, ty, index, output_path, style_dark=False, save_kwargs=None):
    """Renders one 256x256 Toner tile and writes it as a PNG file."""
    im = render_tile_image(z, tx, ty, index, style_dark=style_dark)
    opts = save_kwargs or {}
    if opts.get("bits") == 1:
        dither = getattr(Image, "Dither", None)
        im = im.convert("1", dither=getattr(dither, "NONE", 0) if dither else 0)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    im.save(output_path, format="PNG", **opts)
    return im


def _draw_way(draw, z, code, width, casing, px_pts, fg_color, bg_color):
    """Draws one way.  ``px_pts`` is a flat ``[x0, y0, x1, y1, ...]`` sequence."""
    if code == DRAW_WATER_FILL:
        if len(px_pts) >= 6:
            draw.polygon(px_pts, fill=fg_color)
    elif code == DRAW_WATER_LINE:
        draw.line(px_pts, fill=fg_color, width=width, joint="curve")
    elif code == DRAW_WATER_INNER_FILL:
        # inner ring of a lake (island): punch it back to the background colour.
        if len(px_pts) >= 6:
            draw.polygon(px_pts, fill=bg_color)
    elif code == DRAW_WATER_INNER_LINE:
        # open ring: outline it instead of punching a shape that the rasteriser
        # would close with a straight edge.
        draw.line(px_pts, fill=bg_color, width=width, joint="curve")
    elif code == DRAW_RAILWAY:
        draw.line(px_pts, fill=fg_color, width=1, joint="curve")
    else:
        draw.line(px_pts, fill=fg_color, width=width, joint="curve")
        if casing and z >= 13 and width >= 5:
            draw.line(px_pts, fill=bg_color, width=width - 2, joint="curve")


def label_font(kind, z):
    """Font and dot radius used for a place kind at a given zoom."""
    if kind == 0:                      # city
        return get_font(14, bold=True), 4
    if kind == 1:                      # town
        return get_font(12, bold=True), 3
    if kind in (2, 3) and z >= 13:     # village, suburb
        return get_font(10, bold=True), 2
    if kind in (4, 5) and z >= 14:     # hamlet, neighbourhood
        return get_font(9, bold=False), 2
    return None, 0


def place_score(kind, population):
    if population and population > 0:
        return population
    return PLACE_DEFAULT_SCORE.get(kind, 1000)


def _boxes_overlap(a, b, pad=1):
    return not (a[2] + pad <= b[0] or b[2] + pad <= a[0] or
                a[3] + pad <= b[1] or b[3] + pad <= a[1])


def draw_labels(draw, z, tx, ty, entries, fg_color, bg_color):
    """Places the most important labels that fit, without overlaps.

    The reference Toner style only shows a handful of labels per tile and drops
    the ones that would collide; without this a low zoom tile ends up covered in
    overlapping names.
    """
    kinds = zoom_label_kinds(z)
    budget = label_budget(z)
    candidates = []
    for entry in entries:
        lat, lon, kind, name = entry[0], entry[1], entry[2], entry[3]
        population = entry[4] if len(entry) > 4 else 0
        if kind not in kinds or not name:
            continue
        candidates.append((place_score(kind, population), lat, lon, kind, name))
    if not candidates:
        return
    candidates.sort(key=lambda e: -e[0])

    n = 2.0 ** z
    placed = []
    for _score, lat, lon, kind, name in candidates:
        if len(placed) >= budget:
            break
        font, dot_r = label_font(kind, z)
        if font is None:
            continue
        px = int(round((world_x(lat, lon, n) - tx) * 256.0))
        py = int(round((world_y(lat, lon, n) - ty) * 256.0))
        x = px + dot_r + 3
        y = py - 6
        try:
            tb = draw.textbbox((x, y), name, font=font, stroke_width=2)
        except Exception:  # pragma: no cover - very old Pillow
            tb = (x, y, x + 9 * len(name), y + 14)
        box = (tb[0], tb[1], tb[2], tb[3])
        if box[2] < 0 or box[0] > 256 or box[3] < 0 or box[1] > 256:
            continue
        if any(_boxes_overlap(box, b) for b in placed):
            continue
        placed.append(box)
        draw.ellipse([(px - dot_r, py - dot_r), (px + dot_r, py + dot_r)], fill=fg_color)
        draw.text((x, y), name, fill=fg_color, font=font, stroke_width=2, stroke_fill=bg_color)


def draw_place_label(draw, z, tx, ty, entry, fg_color, bg_color):
    """Single-label helper (kept for compatibility / tests)."""
    draw_labels(draw, z, tx, ty, [entry], fg_color, bg_color)


def render_toner_tile_reference(z, tx, ty, ways, places, output_path, style_dark=False):
    """
    Straightforward (slow) renderer used as the oracle of the test-suite: it
    must apply exactly the same style rules as the fast path, and the fast path
    must reproduce it pixel for pixel.
    """
    n = 2.0 ** z
    bg_color = 0 if style_dark else 255
    fg_color = 255 if style_dark else 0

    im = Image.new("L", (256, 256), bg_color)
    draw = ImageDraw.Draw(im)

    # 1. water: areas / rivers / streams, then the inner rings (islands)
    for phase in ("outer", "inner"):
        for w in ways:
            if w["type"] != "water":
                continue
            kind = legacy_water_kind(w)
            is_inner = kind == WATER_RING_INNER
            if (phase == "inner") != is_inner:
                continue
            if not water_kind_drawn(kind, z):
                continue
            px_pts = [project_pt(lat, lon, tx, ty, n) for lat, lon in w["pts"]]
            if not px_pts:
                continue
            if kind in (WATER_AREA, WATER_RING_OUTER, WATER_RING_INNER):
                xs = [p[0] for p in px_pts]
                ys = [p[1] for p in px_pts]
                if max(max(xs) - min(xs), max(ys) - min(ys)) < water_min_size(z):
                    continue
            color = bg_color if is_inner else fg_color
            if w["closed"] and len(px_pts) >= 3:
                draw.polygon(px_pts, fill=color)
            else:
                draw.line(px_pts, fill=color, width=water_line_width(z), joint="curve")

    # 2. railways
    for w in ways:
        if w["type"] == "railway":
            draw.line([project_pt(lat, lon, tx, ty, n) for lat, lon in w["pts"]],
                      fill=fg_color, width=1, joint="curve")

    # 3. roads, minor -> major (the class order is the drawing order)
    for cls in ROAD_ORDER:
        style = road_style(cls, z)
        if style is None:
            continue
        _, width, is_casing = style
        for w in ways:
            if w["type"] == "highway" and w["class"] == cls:
                px_pts = [project_pt(lat, lon, tx, ty, n) for lat, lon in w["pts"]]
                draw.line(px_pts, fill=fg_color, width=width, joint="curve")
                if is_casing and width >= 3:
                    draw.line(px_pts, fill=bg_color, width=width - 2, joint="curve")

    # 4. labels
    entries = []
    for p in places:
        lat, lon = p["lat"], p["lon"]
        # a label belongs to the tile that contains its anchor (same rule the
        # fast path applies when it builds the per tile buckets)
        if int(math.floor(world_x(lat, lon, n))) != tx:
            continue
        if int(math.floor(world_y(lat, lon, n))) != ty:
            continue
        entries.append((lat, lon, PLACE_KIND_IDX.get(p["type"], 255),
                        p["name"], p.get("population", 0)))
    draw_labels(draw, z, tx, ty, entries, fg_color, bg_color)

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    im.save(output_path, format="PNG", optimize=True)
    return im


def legacy_water_kind(w):
    """Maps a legacy way dict to a water sub-kind."""
    kind = w.get("water_kind")
    if kind is not None:
        return kind
    cls = w.get("class")
    if cls in MINOR_WATERWAYS:
        return WATER_STREAM
    if cls in MAJOR_WATERWAYS:
        return WATER_RIVER
    return WATER_AREA


def water_kind_drawn(kind, z):
    if kind == WATER_RING_OUTER:
        return True
    if kind == WATER_RING_INNER:
        return True
    if kind == WATER_AREA:
        return z >= 10
    if kind == WATER_RIVER:
        return z >= 10
    return z >= 13  # streams


# --------------------------------------------------------------------------- #
# PNG save options
# --------------------------------------------------------------------------- #
PNG_MODES = {
    "fast": {"compress_level": 6},          # ~3 ms/tile, lossless, default
    "small": {"optimize": True},            # ~24 ms/tile, ~10% smaller
    "mono": {"optimize": True, "bits": 1},  # 1-bit e-paper friendly (needs convert)
}


def save_kwargs_for(png_mode):
    return dict(PNG_MODES.get(png_mode, PNG_MODES["fast"]))


# --------------------------------------------------------------------------- #
# Worker plumbing for parallel rendering
# --------------------------------------------------------------------------- #
_WORKER_STATE = {}


def _worker_init(index, save_kwargs, style_dark, output_dir, mode="png", bits=1,
                 deflate_level=6):
    _WORKER_STATE["index"] = index
    _WORKER_STATE["save_kwargs"] = save_kwargs
    _WORKER_STATE["style_dark"] = style_dark
    _WORKER_STATE["out"] = output_dir
    _WORKER_STATE["mode"] = mode
    _WORKER_STATE["bits"] = bits
    _WORKER_STATE["deflate_level"] = deflate_level


def _render_one(task):
    """Renders one tile, encodes it and returns a result record.

    The worker does the encoding (PNG file and/or packed+deflated payload)
    because that is the part that parallelises: the parent only appends the
    finished payload to the container, so the .mmap is written sequentially
    without holding the whole map in memory.

    Returns (z, x, y, png_bytes, packed_payload, blank_kind, render_ms,
    png_ms, pack_ms).
    """
    z, tx, ty = task
    state = _WORKER_STATE
    index = state["index"]
    if isinstance(index, dict):
        index = index[z]
    mode = state.get("mode", "png")

    t0 = time.perf_counter()
    im = render_tile_image(z, tx, ty, index, style_dark=state["style_dark"])
    t1 = time.perf_counter()

    png_bytes = 0
    if mode in ("png", "both"):
        path = os.path.join(state["out"], str(z), str(tx), f"{ty}.png")
        opts = state["save_kwargs"]
        out_im = im
        if opts.get("bits") == 1:
            dither = getattr(Image, "Dither", None)
            out_im = out_im.convert("1", dither=getattr(dither, "NONE", 0) if dither else 0)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        out_im.save(path, format="PNG", **opts)
        png_bytes = os.path.getsize(path)
    t2 = time.perf_counter()

    packed = None
    blank = 0
    if mode in ("mmap", "both"):
        bits = state["bits"]
        raw = pack_tile_image(im, 256, bits, dither=(bits == 2))
        blank = blank_kind(raw)
        packed = b"" if blank else zlib.compress(raw, state["deflate_level"])
    t3 = time.perf_counter()

    return (z, tx, ty, png_bytes, packed, blank,
            (t1 - t0) * 1000.0, (t2 - t1) * 1000.0, (t3 - t2) * 1000.0)


# --------------------------------------------------------------------------- #
# Tile generation pipeline
# --------------------------------------------------------------------------- #
def enumerate_tiles(bbox, zoom_levels):
    """All tiles of the bbox, as an explicit list (only for jobs within the cap)."""
    tiles = []
    for z in zoom_levels:
        x0, x1, y0, y1 = tile_range(bbox, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                tiles.append((z, x, y))
    return tiles


def tile_range(bbox, z):
    """Inclusive tile range (x0, x1, y0, y1) covering the bbox at one zoom."""
    min_lat, min_lon, max_lat, max_lon = bbox
    x_min, y_min = deg2num(max_lat, min_lon, z)
    x_max, y_max = deg2num(min_lat, max_lon, z)
    return (min(x_min, x_max), max(x_min, x_max), min(y_min, y_max), max(y_min, y_max))


def count_tiles(bbox, z):
    """Number of tiles at one zoom, computed without building the list: a
    whole-PBF request at z16 is a million tiles, and materialising that just to
    print the plan is how a pre-flight check turns into an out-of-memory."""
    x0, x1, y0, y1 = tile_range(bbox, z)
    return (x1 - x0 + 1) * (y1 - y0 + 1)


def tile_path(output_dir, z, tx, ty):
    return os.path.join(output_dir, str(z), str(tx), f"{ty}.png")


def load_or_extract(pbf_path, bbox, area_name, cache_dir=None, force=False, verbose=True):
    min_lat, min_lon, max_lat, max_lon = bbox
    if cache_dir is None:
        cache_dir = os.path.join(os.path.dirname(os.path.abspath(pbf_path)), ".pbf_cache")
    # CRC32 over the raw IEEE754 bytes of the bbox: identical to the key used by
    # the native implementation, so both tools share the same cache file.
    key = f"{area_name}_{zlib.crc32(struct.pack('<4d', *bbox)) & 0xFFFFFFFF:08x}"
    cache_file = os.path.join(cache_dir, f"{key}.mosmb")

    if os.path.exists(cache_file) and not force and cache_version(cache_file) == CACHE_VERSION:
        if verbose:
            print(f"[*] Loading vectors from cache: {cache_file}")
        try:
            t0 = time.time()
            data = cache_read(cache_file)
            if verbose:
                print(f"[✓] Cache loaded in {time.time() - t0:.2f}s "
                      f"({data.n_ways} roads, {data.n_places} places)")
            return data
        except Exception as e:
            print(f"[!] Cache read error ({e}), re-extracting...")
    elif os.path.exists(cache_file) and not force and verbose:
        print(f"[*] Cache in old format (v{cache_version(cache_file)}): re-extracting "
              f"to include lakes (relations) and population")

    # Legacy caches written by older versions of the script
    legacy = os.path.join(cache_dir, f"{area_name}.json.gz")
    if os.path.exists(legacy) and not force:
        try:
            if verbose:
                print(f"[*] Converting legacy cache: {legacy}")
            data = cache_load_legacy_json(legacy)
            cache_write(cache_file, data)
            return data
        except Exception as e:
            print(f"[!] Legacy cache read error ({e}), re-extracting...")

    data = extract_area_from_pbf(pbf_path, min_lat, min_lon, max_lat, max_lon, cache_file, verbose=verbose)
    return data


def generate_tiles_from_pbf(pbf_path, min_lat, min_lon, max_lat, max_lon, zoom_levels,
                            output_dir="./tiles", area_name="area", style_dark=False,
                            force=False, jobs=None, png_mode="fast", cache_dir=None,
                            keep_existing=True, verbose=True, out_format="png",
                            mmap_out=None, bits=1, deflate_level=6, benchmark=False,
                            plan_only=False, max_tiles=200000, whole_pbf=False,
                            no_poi=False):
    bbox = (min_lat, min_lon, max_lat, max_lon)
    t_pipeline = time.time()
    use_png = out_format in ("png", "both")
    use_mmap = out_format in ("mmap", "both")

    if jobs is None:
        jobs = max(1, (os.cpu_count() or 2) - 1)
    jobs = max(1, int(jobs))

    # Pre-flight plan: tile counts and size are known before the (expensive)
    # extraction, so a "whole PBF" request cannot turn into a surprise overnight job.
    # The counts are computed arithmetically: building the list first would need
    # gigabytes for a whole-PBF z16 request.
    plan = tile_plan(bbox, zoom_levels, bits=bits)
    total_count = plan["total"]
    if verbose:
        print_tile_plan(plan, jobs, out_format, bits, whole_pbf=whole_pbf, bbox=bbox)
    if max_tiles and total_count > max_tiles:
        raise SystemExit(
            f"[!] The plan requires {total_count:,} tiles, over the limit of {max_tiles:,} "
            f"(--max-tiles to change it, 0 for no limit).\n"
            f"    Hint: reduce --zooms or generate one region at a time.")
    if plan_only:
        return {"rendered": 0, "skipped": 0, "bytes": 0, "seconds": 0.0,
                "pipeline_seconds": 0.0, "jobs": jobs, "mmap": None, "stats": {},
                "extract_seconds": 0.0, "index_seconds": 0.0,
                "plan": plan, "planned_only": True}

    t_extract = time.time()
    data = load_or_extract(pbf_path, bbox, area_name, cache_dir=cache_dir, force=force, verbose=verbose)
    extract_time = time.time() - t_extract

    # Cheap win: never re-render tiles that already exist unless forced. Only
    # for the PNG output: a .mmap has no per-tile file to check, so the whole
    # grid is always rendered when a container is requested.  The tiles stay
    # grouped per zoom: the z16 grid of a whole-PBF run alone is a million
    # tasks, and keeping every zoom alive at once was a six-figure-MB list.
    skipped = 0
    pending = {}
    for z in zoom_levels:
        if keep_existing and not force and out_format == "png":
            tiles = [t for t in enumerate_tiles(bbox, [z])
                     if not os.path.exists(tile_path(output_dir, z, t[1], t[2]))]
            skipped += count_tiles(bbox, z) - len(tiles)
        else:
            tiles = enumerate_tiles(bbox, [z])
        if tiles:
            pending[z] = tiles
    if skipped and verbose:
        print(f"[*] {skipped} tiles already present: skipped (use --force to regenerate them).")

    if not pending:
        if verbose:
            print("[✓] Nothing to render.")
        return {"rendered": 0, "skipped": skipped, "bytes": 0, "seconds": 0.0}

    total_count = sum(len(tiles) for tiles in pending.values())
    if verbose:
        print(f"\n[*] Rendering {total_count} Toner tiles (Zoom {zoom_levels})...")

    needed_zooms = sorted(pending)

    # Directories are created once per zoom, not once per tile: only when PNGs
    # are actually written (a .mmap run would leave a tree of empty folders).
    if use_png:
        for z in needed_zooms:
            for d in {os.path.join(output_dir, str(z), str(tx)) for _z, tx, _ty in pending[z]}:
                os.makedirs(d, exist_ok=True)

    total_bytes = 0

    # Build the per-zoom indexes once. With the fork start method the children
    # inherit them copy-on-write, so no pickling and no duplicated memory; the
    # compact CSR layout keeps Python objects out of the shared pages, so the
    # workers do not even trigger copy-on-write faults while rendering.
    if verbose:
        print(f"[*] Indexing geometries (zoom {needed_zooms})...")
    t_idx = time.time()
    ref_z = max(needed_zooms)
    # The requested zooms decide which ways are worth projecting at all, and the
    # per-way bounding boxes are measured once for all of them (a single zoom
    # pays for the extra pass without using it twice).
    locations = project_ways_at_zoom(data, ref_z, zooms=needed_zooms)
    boxes = projected_way_boxes(data, locations, zooms=needed_zooms) \
        if len(needed_zooms) > 1 else None
    way_off = _way_offsets(data)
    index = {}
    for z in needed_zooms:
        index[z] = build_tile_index(data, z, locations=locations, boxes=boxes,
                                    way_off=way_off)
    if verbose:
        print(f"[✓] Index ready in {time.time() - t_idx:.2f}s")
    index_time = time.time() - t_idx
    del locations, boxes

    save_kwargs = save_kwargs_for(png_mode)
    worker_mode = out_format if out_format in ("png", "mmap", "both") else "png"

    writer = None
    mmap_target = ""
    if use_mmap:
        mmap_target = mmap_out or os.path.join(output_dir, f"{area_name}.mmap")
        os.makedirs(os.path.dirname(os.path.abspath(mmap_target)) or ".", exist_ok=True)
        writer = MapWriter(mmap_target, bits=bits, tile_px=256, name=area_name,
                           bbox=bbox, dark=style_dark, deflate_level=deflate_level)
        # Place index: enables on-device search and label work later on, costs
        # ~20 bytes per place plus the names blob (68k places for the whole
        # north-west PBF: skip it with --no-poi if size matters more than that).
        if not no_poi:
            for i in range(getattr(data, "n_places", 0)):
                kind_idx = data.place_kind[i]
                writer.add_poi(data.place_lat_e5[i] / E5, data.place_lon_e5[i] / E5,
                               PLACE_KINDS[kind_idx] if kind_idx < len(PLACE_KINDS) else "other",
                               data.place_population[i], data.place_names[i])
        if verbose:
            print(f"[*] .mmap container: {mmap_target} "
                  f"({bits} bit/pixel, deflate {deflate_level}, "
                  f"{0 if no_poi else getattr(data, 'n_places', 0)} POI)")

    # The renderer works from the compact indexes alone: release the vector
    # cache (hundreds of MB on a whole-PBF run) before the workers are forked.
    del data

    stats = {"render_ms": 0.0, "png_ms": 0.0, "pack_ms": 0.0, "tiles": 0, "collect_s": 0.0}

    def _collect(result):
        """Feeds one finished tile into the requested outputs."""
        z, tx, ty = result[0], result[1], result[2]
        nonlocal total_bytes
        if use_png:
            total_bytes += result[3]
        stats["render_ms"] += result[6]
        stats["png_ms"] += result[7]
        stats["pack_ms"] += result[8]
        if writer is not None:
            t0 = time.perf_counter()
            writer.add_tile_compressed(z, tx, ty, result[4] or b"", result[5])
            stats["collect_s"] += time.perf_counter() - t0
        stats["tiles"] += 1

    def _report(idx, total):
        if verbose and (idx % 25 == 0 or idx == total):
            print(f"[{idx}/{total}] tiles rendered")

    # The clock starts here: everything above (extraction, index, container
    # setup) is reported on its own line, so "rendering" below means rendering.
    t_start = time.time()
    use_rich_progress = verbose and HAS_RICH and sys.stdout.isatty() and total_count > 0

    def _render_batch(z, tiles):
        """Renders one zoom and feeds the results into the outputs.

        The pool is recreated per zoom on purpose: the index of a zoom is
        dropped as soon as its tiles are done, so the peak is one zoom at a
        time instead of the whole zoom pyramid at once.
        """
        idx = index[z]
        _worker_init(idx, save_kwargs, style_dark, output_dir, worker_mode, bits,
                     deflate_level)
        if jobs > 1 and len(tiles) > 3:
            try:
                ctx = mp.get_context("fork")
                fork_ctx = True
            except ValueError:  # pragma: no cover - non-fork platforms
                ctx = mp.get_context()
                fork_ctx = False
            # With fork the children inherit the index copy-on-write: no
            # pickling of the (large) vector data at all.
            if fork_ctx:
                pool_ctx = ctx.Pool(processes=jobs)
            else:  # pragma: no cover - non-fork platforms
                pool_ctx = ctx.Pool(processes=jobs, initializer=_worker_init,
                                    initargs=(idx, save_kwargs, style_dark, output_dir,
                                              worker_mode, bits, deflate_level))
            with pool_ctx as pool:
                if use_rich_progress:
                    with Progress(
                        SpinnerColumn(),
                        TextColumn("[progress.description]{task.description}"),
                        BarColumn(),
                        TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
                        TextColumn("({task.completed}/{task.total})"),
                        TimeRemainingColumn(),
                        console=console
                    ) as progress:
                        task = progress.add_task(
                            f"[cyan]Rendering Toner vector tiles z{z}...", total=len(tiles))
                        for result in pool.imap_unordered(_render_one, tiles, chunksize=8):
                            _collect(result)
                            progress.advance(task)
                else:
                    for result in pool.imap_unordered(_render_one, tiles, chunksize=8):
                        _collect(result)
                        _report(stats["tiles"], total_count)
        else:
            if use_rich_progress:
                with Progress(
                    SpinnerColumn(),
                    TextColumn("[progress.description]{task.description}"),
                    BarColumn(),
                    TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
                    TextColumn("({task.completed}/{task.total})"),
                    TimeRemainingColumn(),
                    console=console
                ) as progress:
                    task = progress.add_task(
                        f"[cyan]Rendering Toner vector tiles z{z}...", total=len(tiles))
                    for t in tiles:
                        _collect(_render_one(t))
                        progress.advance(task)
            else:
                for t in tiles:
                    _collect(_render_one(t))
                    _report(stats["tiles"], total_count)

    mmap_stats = None
    mmap_seconds = 0.0
    writer_closed = False
    try:
        for z in needed_zooms:
            _render_batch(z, pending[z])
            # Free the zoom before the next one: its workers are gone and the
            # inherited pages die with them, so the parent can release both the
            # index and the task list outright instead of stacking every zoom.
            del pending[z], index[z]
            _WORKER_STATE.clear()
        render_time = time.time() - t_start
        if writer is not None:
            t_write = time.time()
            mmap_stats = writer.close()
            writer_closed = True
            mmap_seconds = time.time() - t_write
    finally:
        # A failed run must not leave a multi-hundred-MB payload temp behind.
        if writer is not None and not writer_closed:
            writer.abort()
    pipeline_time = time.time() - t_pipeline
    rendered = stats["tiles"]
    mb = total_bytes / (1024 * 1024)

    parts = [
        "[✓] Toner rendering completed successfully!",
        f"    Tiles generated: {rendered} in {render_time:.2f}s "
        f"({rendered / max(0.01, render_time):.1f} tile/s)",
        f"    Total time (index + rendering + writing): {pipeline_time:.2f}s with {jobs} processes",
    ]
    if use_png:
        parts.append(f"    PNG: {mb:.2f} MB (~{total_bytes / max(1, rendered) / 1024:.1f} KB/tile) "
                     f"in {os.path.abspath(output_dir)}")
    if mmap_stats:
        zooms = ", ".join(f"z{z}:{n}" for z, n in sorted(mmap_stats["zooms"].items()))
        parts += [
            f"    Container: {mmap_stats['bytes'] / 1e6:.2f} MB "
            f"({mmap_stats['payload_bytes'] / max(1, mmap_stats['tiles']):.0f} B/compressed tile, "
            f"{mmap_stats['blank']} blank tiles, {mmap_stats['pois']} POI)",
            f"    Zooms in the container: {zooms}",
            f"    Container write: {mmap_seconds:.2f}s",
        ]
    parts.append("")
    parts.append("[*] How to use on M5Stack Paper Mono:")
    if use_png:
        parts.append(f"    Copy the folder '{os.path.basename(output_dir)}' to the root of the MicroSD:")
        parts.append("    Final path on MicroSD: /sdcard/tiles/{z}/{x}/{y}.png")
    if mmap_stats:
        parts.append(f"    Copy '{os.path.basename(mmap_target)}' (a single file) into the /maps folder of the MicroSD:")
        parts.append("    Final path on MicroSD: /sdcard/maps/<region>.mmap")
    parts += [
        "",
        "[*] Legal notes / Attribution:",
        "    Map data: © OpenStreetMap contributors (ODbL)",
        "    Style: Stamen Toner B/W E-Paper Style",
    ]
    summary_text = "\n".join(parts)

    if verbose and HAS_RICH:
        console.print(Panel(summary_text, title="[bold green]MonoMesh Offline Map Builder[/bold green]", expand=False))
    elif verbose:
        print("\n" + "=" * 65)
        print(summary_text.replace("[✓]", "+").replace("[*]", "*"))
        print("=" * 65 + "\n")

    if benchmark:
        rows = [
            ("extraction/cache", extract_time, ""),
            ("vector index", index_time, ""),
            ("rendering (wall)", render_time,
             f"{rendered / max(0.01, render_time):.1f} tile/s on {jobs} processes"),
            ("  render CPU (sum)", stats["render_ms"] / 1000.0,
             f"{stats['render_ms'] / max(1, rendered):.2f} ms/tile"),
        ]
        if use_png:
            rows.append(("  PNG encode (sum)", stats["png_ms"] / 1000.0,
                         f"{stats['png_ms'] / max(1, rendered):.2f} ms/tile, {mb:.1f} MB"))
        if mmap_stats:
            rows.append(("  pack+deflate (sum)", stats["pack_ms"] / 1000.0,
                         f"{stats['pack_ms'] / max(1, rendered):.2f} ms/tile"))
            rows.append(("container write", mmap_seconds,
                         f"{mmap_stats['bytes'] / 1e6:.1f} MB, "
                         f"{mmap_stats['bytes'] / max(0.001, mmap_seconds) / 1e6:.1f} MB/s"))
        rows.append(("pipeline total", pipeline_time, ""))
        print("[*] Generation benchmark")
        for name, secs, note in rows:
            print(f"    {name:<22} {secs:8.2f} s   {note}")

    return {"rendered": rendered, "skipped": skipped, "bytes": total_bytes,
            "seconds": render_time, "pipeline_seconds": pipeline_time, "jobs": jobs,
            "mmap": mmap_stats, "stats": stats, "extract_seconds": extract_time,
            "index_seconds": index_time}


# --------------------------------------------------------------------------- #
# Interactive TUI
# --------------------------------------------------------------------------- #
def run_tui():
    header = (
        "[bold cyan]===========================================================[/bold cyan]\n"
        "[bold white]  MonoMesh Offline PBF Tile Generator (Toner E-Paper)    [/bold white]\n"
        "[bold cyan]===========================================================[/bold cyan]"
    )
    if HAS_RICH:
        console.print(header)
    else:
        print("\n" + "=" * 60)
        print("  MonoMesh Offline PBF Tile Generator (Toner E-Paper)")
        print("=" * 60 + "\n")

    # 1. Detect PBF files
    pbf_candidates = find_pbf_files()
    if not pbf_candidates:
        print("[!] No .osm.pbf file found in the current folder!")
        pbf_path = input("Enter the full path to the .osm.pbf file: ").strip()
    elif len(pbf_candidates) == 1:
        pbf_path = pbf_candidates[0]
        print(f"[✓] PBF file detected: {os.path.basename(pbf_path)} ({os.path.getsize(pbf_path)/(1024*1024):.0f} MB)")
    else:
        print("\nPBF files found:")
        for idx, p in enumerate(pbf_candidates, 1):
            print(f"  [{idx}] {os.path.basename(p)} ({os.path.getsize(p)/(1024*1024):.0f} MB)")
        sel = input("Choose the PBF file [default: 1]: ").strip() or "1"
        pbf_path = pbf_candidates[int(sel) - 1]

    if not os.path.exists(pbf_path):
        print(f"[!] File {pbf_path} does not exist!")
        return

    # 2. Select Geographic Area
    print("\nChoose how to define the area to extract:")
    print("  [1] Search by city / municipality name (e.g. 'Como', 'Milano', 'Varese', 'Lecco')")
    print("  [2] Manual coordinates (latitude, longitude and radius in km)")
    print("  [3] Manual bounding box (North, South, West, East)")
    print("  [4] Whole .osm.pbf file (bounding box declared in the header)")

    choice = input("\nChoice [1..4, default: 1]: ").strip() or "1"
    area_name = "area"
    whole_pbf = (choice == "4")

    if whole_pbf:
        hb = pbf_header_bbox(pbf_path)
        if not hb:
            print("[!] The PBF file does not declare a bounding box: using Como.")
            min_lat, max_lat, min_lon, max_lon = 45.77, 45.85, 9.04, 9.13
            area_name = "como"
            whole_pbf = False
        else:
            min_lat, min_lon, max_lat, max_lon = hb
            area_name = os.path.splitext(os.path.basename(pbf_path))[0]
            print(f"[✓] Area = whole file: {min_lat:.2f}..{max_lat:.2f} N, "
                  f"{min_lon:.2f}..{max_lon:.2f} E")
    elif choice == "1":
        city_name = input("City or municipality name: ").strip() or "Como"
        area_name = city_name.strip().lower().replace(" ", "_")
        res = geocode_city(city_name)
        if res:
            print(f"[✓] Found: {res['name']} ({res['lat']:.4f}, {res['lon']:.4f})")
            r_str = input("Radius in km to cover [default: 4.0]: ").strip() or "4.0"
            r_km = float(r_str)
            dlat = r_km / 111.0
            dlon = r_km / (111.0 * math.cos(math.radians(res["lat"])))
            min_lat, max_lat = res["lat"] - dlat, res["lat"] + dlat
            min_lon, max_lon = res["lon"] - dlon, res["lon"] + dlon
            area_name = f"{area_name}_r{int(r_km)}km"
        else:
            print("[!] City not found. Using Como.")
            min_lat, max_lat, min_lon, max_lon = 45.77, 45.85, 9.04, 9.13
            area_name = "como"
    elif choice == "2":
        lat = float(input("Latitude (e.g. 45.8081): ").strip() or "45.8081")
        lon = float(input("Longitude (e.g. 9.0852): ").strip() or "9.0852")
        r_km = float(input("Radius in km [default: 4.0]: ").strip() or "4.0")
        dlat = r_km / 111.0
        dlon = r_km / (111.0 * math.cos(math.radians(lat)))
        min_lat, max_lat = lat - dlat, lat + dlat
        min_lon, max_lon = lon - dlon, lon + dlon
        area_name = f"lat{lat:.2f}_lon{lon:.2f}_r{int(r_km)}km"
    else:
        # One label per value: the old prompts said "Nord, Sud, Ovest, Est" but read
        # min_lat, max_lat, min_lon, max_lon, which is an easy way to ask for a
        # continent by accident.
        print("Bounding box in decimal degrees:")
        min_lat = float(input("  Minimum latitude (South): ").strip())
        max_lat = float(input("  Maximum latitude (North): ").strip())
        min_lon = float(input("  Minimum longitude (West): ").strip())
        max_lon = float(input("  Maximum longitude (East): ").strip())
        area_name = f"bbox_{min_lat:.2f}_{min_lon:.2f}"

    if not (min_lat < max_lat and min_lon < max_lon) or \
       not (-90.0 <= min_lat <= 90.0 and -90.0 <= max_lat <= 90.0 and
            -180.0 <= min_lon <= 180.0 and -180.0 <= max_lon <= 180.0):
        print("[!] Invalid bounding box: min_lat < max_lat and min_lon < max_lon are required "
              "(decimal degrees), within the planet's limits.")
        return

    # 3. Zoom levels
    print("\nZoom Levels:")
    print("  - Z11: Overview (~1-2 tiles)")
    print("  - Z12: General view (~4-8 tiles)")
    print("  - Z13: City and main roads (~10-25 tiles, RECOMMENDED)")
    print("  - Z14: Street and hamlet detail (~30-60 tiles, RECOMMENDED)")
    print("  - Z15: Block-level detail (~80+ tiles)")
    z_str = input("Enter zoom levels separated by commas [default: 12,13,14]: ").strip() or "12,13,14"
    zooms = [int(z.strip()) for z in z_str.split(",") if z.strip()]

    # 4. Style
    print("\nToner Style:")
    print("  [1] Pure Toner B/W (E-Paper recommended - white background, black roads/water)")
    print("  [2] Inverted Toner Dark (black background, white roads)")
    st_choice = input("Style choice [1..2, default: 1]: ").strip() or "1"
    style_dark = (st_choice == "2")

    # 5. Performance options
    default_jobs = max(1, (os.cpu_count() or 2) - 1)
    jobs_str = input(f"\nParallel processes [default: {default_jobs}, 1 = serial]: ").strip()
    try:
        jobs = int(jobs_str) if jobs_str else default_jobs
    except ValueError:
        jobs = default_jobs

    out_format = "mmap"
    png_mode = "fast"

    bits = 1
    if out_format in ("mmap", "both"):
        print("\n.mmap Container:")
        bits_choice = input("Bits per pixel in the container "
                            "[1 = B/W, 2 = 4 levels (antialiased labels), default: 1]: ").strip() or "1"
        bits = 2 if bits_choice == "2" else 1

    default_out, out_label = "./maps", "Container destination folder"
    out_dir = input(f"\n{out_label} [default: {default_out}]: ").strip() or default_out
    force_choice = input("Overwrite existing tiles if present? (y/N): ").strip().lower()
    force = force_choice in ("y", "yes")

    plan = tile_plan((min_lat, min_lon, max_lat, max_lon), zooms, bits=bits)
    print_tile_plan(plan, jobs, out_format, bits, whole_pbf=whole_pbf,
                    bbox=(min_lat, min_lon, max_lat, max_lon))
    if plan["total"] > 5000:
        ok = input(f"\nConfirm the generation of {plan['total']:,} tiles? (y/N): ").strip().lower()
        if ok not in ("y", "yes"):
            print("[!] Operation cancelled.")
            return

    generate_tiles_from_pbf(
        pbf_path=pbf_path,
        min_lat=min_lat,
        min_lon=min_lon,
        max_lat=max_lat,
        max_lon=max_lon,
        zoom_levels=zooms,
        output_dir=out_dir,
        area_name=area_name,
        style_dark=style_dark,
        force=force,
        jobs=jobs,
        png_mode=png_mode,
        keep_existing=not force,
        out_format=out_format,
        bits=bits,
        whole_pbf=whole_pbf,
        max_tiles=0,   # gia' confermato qui sopra
    )


def main():
    parser = argparse.ArgumentParser(description="MonoMesh Offline PBF Tile Generator (Toner E-Paper)")
    parser.add_argument("--tui", action="store_true", help="Start the interactive TUI")
    parser.add_argument("--pbf", type=str, default="", help="Path to the .osm.pbf file")
    parser.add_argument("--city", type=str, help="City name (e.g. 'Como', 'Milano')")
    parser.add_argument("--lat", type=float, help="Center latitude")
    parser.add_argument("--lon", type=float, help="Center longitude")
    parser.add_argument("--radius_km", type=float, default=4.0, help="Radius in km (default: 4.0)")
    parser.add_argument("--bbox", nargs=4, type=float, metavar=("MIN_LAT", "MIN_LON", "MAX_LAT", "MAX_LON"),
                        help="Bounding box: min_lat min_lon max_lat max_lon")
    parser.add_argument("--zooms", type=str, default="12,13,14", help="Comma-separated zoom levels (default: 12,13,14)")
    parser.add_argument("--dark", action="store_true", help="Dark / inverted Toner style")
    parser.add_argument("--output", type=str, default=None,
                        help="Destination folder (default: ./maps for a .mmap container, ./tiles for PNG)")
    parser.add_argument("--force", action="store_true", help="Force regeneration (ignore cache and existing tiles)")
    parser.add_argument("--jobs", type=int, default=None, help="Parallel processes (default: CPU-1)")
    parser.add_argument("--png-mode", choices=sorted(PNG_MODES), default="fast",
                        help="PNG mode: fast (default), small, mono (only with --format png/both)")
    parser.add_argument("--bits", type=int, choices=[1, 2], default=1,
                        help="Bits per pixel in the container: 1 = B/W, 2 = 4 levels (only with --format mmap/both)")
    parser.add_argument("--format", choices=["png", "mmap", "both"], default="mmap",
                        help="Output: mmap (default, single container), png, both")
    parser.add_argument("--mmap-out", type=str, default=None,
                        help="Path to the .mmap container (default <output>/<area>.mmap)")
    parser.add_argument("--deflate-level", type=int, default=6,
                        help="zlib compression level of the payload (default 6)")
    parser.add_argument("--benchmark", action="store_true",
                        help="Print per-phase timings (rendering, packing, writing)")
    parser.add_argument("--whole-pbf", action="store_true",
                        help="Use the whole .osm.pbf file as the area (bounding box from the header)")
    parser.add_argument("--plan", action="store_true",
                        help="Show only the plan (tiles per zoom, estimated size) and exit")
    parser.add_argument("--max-tiles", type=int, default=200000,
                        help="Safety limit on the number of tiles per run (0 = no limit)")
    parser.add_argument("--no-poi", action="store_true",
                        help="Do not include the place index in the container")
    parser.add_argument("--cache-dir", type=str, default=None, help="Cache folder (default: <pbf dir>/.pbf_cache)")
    parser.add_argument("--quiet", action="store_true", help="Reduce output")

    args = parser.parse_args()

    # Options that belong to the other output format: say it out loud instead of
    # silently ignoring them (the .mmap has its own encoding, the PNGs have theirs).
    if args.format == "mmap":
        if args.png_mode != "fast":
            print("[i] --png-mode only applies to PNGs: with --format mmap it is ignored "
                  "(use --bits for the container levels)")
    elif args.format == "png":
        if args.bits != 1:
            print("[i] --bits only applies to the container: with --format png it is ignored")
        if args.no_poi:
            print("[i] --no-poi only applies to the container: with --format png it is ignored")

    # Interactive TUI mode if no area specified
    if args.tui or (args.city is None and args.lat is None and args.bbox is None and len(sys.argv) == 1):
        try:
            run_tui()
        except (KeyboardInterrupt, EOFError):
            print("\n[!] Operation cancelled by the user.")
        return

    # Find PBF
    pbf_path = args.pbf
    if not pbf_path:
        candidates = find_pbf_files()
        if candidates:
            pbf_path = candidates[0]
        else:
            print("[!] No .osm.pbf file found! Specify the path with --pbf <file.pbf>")
            sys.exit(1)

    area_name = "area"
    whole_pbf = bool(args.whole_pbf)
    if whole_pbf:
        hb = pbf_header_bbox(pbf_path)
        if not hb:
            print("[!] The PBF file does not declare a bounding box in the header: use --bbox or --city.")
            sys.exit(1)
        min_lat, min_lon, max_lat, max_lon = hb
        area_name = os.path.splitext(os.path.basename(pbf_path))[0]
    elif args.city:
        res = geocode_city(args.city)
        if not res:
            print(f"[!] Cannot geocode '{args.city}'.")
            sys.exit(1)
        lat, lon = res["lat"], res["lon"]
        dlat = args.radius_km / 111.0
        dlon = args.radius_km / (111.0 * math.cos(math.radians(lat)))
        min_lat, max_lat = lat - dlat, lat + dlat
        min_lon, max_lon = lon - dlon, lon + dlon
        area_name = f"{args.city.lower()}_r{int(args.radius_km)}km"
    elif args.bbox:
        min_lat, min_lon, max_lat, max_lon = args.bbox
        area_name = f"bbox_{min_lat:.2f}_{min_lon:.2f}"
    elif args.lat is not None and args.lon is not None:
        dlat = args.radius_km / 111.0
        dlon = args.radius_km / (111.0 * math.cos(math.radians(args.lat)))
        min_lat, max_lat = args.lat - dlat, args.lat + dlat
        min_lon, max_lon = args.lon - dlon, args.lon + dlon
        area_name = f"lat{args.lat:.2f}_lon{args.lon:.2f}_r{int(args.radius_km)}km"
    else:
        # Default Como
        min_lat, max_lat, min_lon, max_lon = 45.77, 45.85, 9.04, 9.13
        area_name = "como_default"

    zooms = [int(z.strip()) for z in args.zooms.split(",") if z.strip()]

    # The container is a single file that goes into /maps on the card, so by default it is written
    # into ./maps instead of ./tiles. The PNG tree keeps ./tiles (and "both" puts the container next
    # to the tree, as before). An explicit --output always wins.
    out_dir = args.output
    if not out_dir:
        out_dir = "./tiles" if args.format in ("png", "both") else "./maps"

    generate_tiles_from_pbf(
        pbf_path=pbf_path,
        min_lat=min_lat,
        min_lon=min_lon,
        max_lat=max_lat,
        max_lon=max_lon,
        zoom_levels=zooms,
        output_dir=out_dir,
        area_name=area_name,
        style_dark=args.dark,
        force=args.force,
        jobs=args.jobs,
        png_mode=args.png_mode,
        cache_dir=args.cache_dir,
        keep_existing=not args.force,
        out_format=args.format,
        mmap_out=args.mmap_out,
        bits=args.bits,
        deflate_level=args.deflate_level,
        benchmark=args.benchmark,
        plan_only=args.plan,
        max_tiles=args.max_tiles,
        whole_pbf=whole_pbf,
        no_poi=args.no_poi,
        verbose=not args.quiet,
    )


if __name__ == "__main__":
    main()
