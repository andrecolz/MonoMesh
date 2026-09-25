#!/usr/bin/env python3
"""Global z0-7 courtesy basemap from Natural Earth (public domain).

The detailed .mmap regions are generated from OSM and cover a city or, at most,
a country. This script builds the small global fallback the firmware uses when
the user pans outside every region: land in white, ocean in solid black (the
low-zoom Toner look) and national borders as black hairlines. Natural Earth is
public domain, so the output can ship with the device.

Data are downloaded once (zip and extracted shapefile) into the cache
directory, then rendered through the same web-mercator conventions as
mmap_converter and packed with the regular MapWriter.
"""

from __future__ import annotations

import argparse
import io
import math
import os
import sys
import time
import zipfile

from PIL import Image, ImageDraw

try:
    from mono_map import MapWriter, pack_tile_image, blank_kind
except ImportError:  # pragma: no cover - only when imported from elsewhere
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mono_map import MapWriter, pack_tile_image, blank_kind

REF_ZOOM = 7                    # shapes are projected once at this zoom
WORLD_LAT = 85.05112878         # web-mercator latitude limit
TILE_PX = 256

NE_URL_TEMPLATES = (
    "https://naciscdn.org/naturalearth/{res}/{category}/{name}.zip",
    "https://naturalearth.s3.amazonaws.com/{res}_{category}/{name}.zip",
)


def download_natural_earth(name, category, resolution, cache_dir):
    """Returns the .shp path, downloading and extracting the zip when missing."""
    import requests

    root = os.path.join(cache_dir, "naturalearth", f"{resolution}_{category}", name)
    shp = os.path.join(root, name + ".shp")
    if os.path.exists(shp):
        return shp
    os.makedirs(root, exist_ok=True)
    last_error = "no url tried"
    for template in NE_URL_TEMPLATES:
        url = template.format(res=resolution, category=category, name=name)
        try:
            resp = requests.get(url, timeout=120)
            if resp.status_code != 200:
                last_error = f"{url}: HTTP {resp.status_code}"
                continue
            with zipfile.ZipFile(io.BytesIO(resp.content)) as zf:
                zf.extractall(root)
            if os.path.exists(shp):
                return shp
            last_error = f"{url}: archive without {name}.shp"
        except Exception as err:   # network error, bad zip...
            last_error = f"{url}: {err}"
    raise SystemExit(f"[!] cannot download {name}: {last_error}")


def load_shapes(path):
    """Reads a shapefile into ``(points, parts)`` tuples (points are lon/lat)."""
    import shapefile

    shapes = []
    for shape in shapefile.Reader(path).shapes():
        points = list(shape.points)
        if points:
            shapes.append((points, list(shape.parts)))
    return shapes


def _project(points, world):
    """Lon/lat -> global pixel coordinates, continuous across the dateline.

    Consecutive longitudes are kept within 180 degrees of each other so a
    polygon that crosses the antimeridian fills on one side instead of wrapping
    across the whole map. Latitudes are clamped to the mercator limit.
    """
    out = []
    prev_lon = None
    for lon, lat in points:
        if prev_lon is not None:
            while lon - prev_lon > 180.0:
                lon -= 360.0
            while lon - prev_lon < -180.0:
                lon += 360.0
        prev_lon = lon
        lat = max(-WORLD_LAT, min(WORLD_LAT, lat))
        x = (lon + 180.0) / 360.0 * world
        y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * world
        out.append((x, y))
    return out


def project_shapes(shapes):
    """Projects ``(points, parts)`` shapes to REF_ZOOM pixels with global bounds.

    Returns ``[(rings, bbox), ...]`` so the tile buckets only need a rectangle
    test before drawing.
    """
    world = float((1 << REF_ZOOM) * TILE_PX)
    out = []
    for points, parts in shapes:
        starts = list(parts) + [len(points)]
        rings = []
        for i in range(len(parts)):
            ring = _project(points[starts[i]:starts[i + 1]], world)
            if len(ring) >= 2:
                rings.append(ring)
        if not rings:
            continue
        xs0 = min(p[0] for ring in rings for p in ring)
        xs1 = max(p[0] for ring in rings for p in ring)
        ys0 = min(p[1] for ring in rings for p in ring)
        ys1 = max(p[1] for ring in rings for p in ring)
        out.append((rings, (xs0, ys0, xs1, ys1)))
    return out


def tile_buckets(shapes, z):
    """``{(tx, ty): [shape index, ...]}`` for one zoom, via the shape bboxes."""
    n = 1 << z
    shift = 8 + (REF_ZOOM - z)
    buckets = {}
    for i, (_rings, bbox) in enumerate(shapes):
        tx0 = max(0, int(math.floor(bbox[0])) >> shift)
        tx1 = min(n - 1, int(math.floor(bbox[2])) >> shift)
        ty0 = max(0, int(math.floor(bbox[1])) >> shift)
        ty1 = min(n - 1, int(math.floor(bbox[3])) >> shift)
        for ty in range(ty0, ty1 + 1):
            for tx in range(tx0, tx1 + 1):
                buckets.setdefault((tx, ty), []).append(i)
    return buckets


def build_basemap(land_shapes, boundary_shapes, out_path, zoom_levels=None,
                  name="basemap", deflate_level=6, verbose=True):
    """Renders the basemap container from geographic shapes.

    ``land_shapes``/``boundary_shapes`` are lists of ``(points, parts)`` in
    lon/lat (see :func:`load_shapes`); the test-suite feeds synthetic ones so no
    download is needed. Returns the MapWriter statistics.
    """
    zooms = sorted(zoom_levels if zoom_levels is not None else range(REF_ZOOM + 1))
    land = project_shapes(land_shapes)
    boundaries = project_shapes(boundary_shapes)
    bbox = (-WORLD_LAT, -180.0, WORLD_LAT, 180.0)
    writer = MapWriter(out_path, bits=1, tile_px=TILE_PX, name=name, bbox=bbox,
                       attribution="Natural Earth (public domain)",
                       deflate_level=deflate_level,
                       tile_grids={z: (0, 0, 1 << z, 1 << z) for z in zooms})
    t0 = time.perf_counter()
    written = False
    try:
        for z in zooms:
            scale = float(1 << (REF_ZOOM - z))
            land_tiles = tile_buckets(land, z)
            border_tiles = tile_buckets(boundaries, z)
            for ty in range(1 << z):
                for tx in range(1 << z):
                    land_idx = land_tiles.get((tx, ty))
                    border_idx = border_tiles.get((tx, ty))
                    if not land_idx and not border_idx:
                        # Deep ocean: nothing to draw, the tile is solid black.
                        writer.add_tile_compressed(z, tx, ty, b"", blank=2)
                        continue
                    ox, oy = tx * TILE_PX, ty * TILE_PX
                    im = Image.new("L", (TILE_PX, TILE_PX), 0)
                    draw = ImageDraw.Draw(im)
                    for i in land_idx or ():
                        for ring in land[i][0]:
                            draw.polygon([(p[0] / scale - ox, p[1] / scale - oy)
                                          for p in ring], fill=255)
                    for i in border_idx or ():
                        for ring in boundaries[i][0]:
                            draw.line([(p[0] / scale - ox, p[1] / scale - oy)
                                       for p in ring], fill=0, width=1, joint="curve")
                    packed = pack_tile_image(im, TILE_PX, 1)
                    writer.add_tile_bits(z, tx, ty, packed, blank=blank_kind(packed))
            if verbose:
                print(f"[*] z{z}: {(1 << z) ** 2} tiles "
                      f"({time.perf_counter() - t0:.1f}s)")
        stats = writer.close()
        written = True
    finally:
        if not written:
            writer.abort()
    if verbose:
        print(f"[OK] {out_path}: {stats['tiles']} tiles, {stats['blank']} blank, "
              f"{stats['bytes'] / 1e6:.2f} MB in {time.perf_counter() - t0:.1f}s")
    return stats


def _parse_zooms(text):
    zooms = set()
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            first, last = part.split("-", 1)
            zooms.update(range(int(first), int(last) + 1))
        else:
            zooms.add(int(part))
    return sorted(z for z in zooms if 0 <= z <= REF_ZOOM)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Build the global z0-7 courtesy basemap from Natural Earth (public domain)")
    ap.add_argument("--out", default="./maps/basemap.mmap")
    ap.add_argument("--cache-dir", default=None,
                    help="data cache (default: <repo>/.pbf_cache)")
    ap.add_argument("--resolution", choices=["110m", "50m"], default="110m",
                    help="Natural Earth detail: 110m is ~70 KB and plenty for z0-7")
    ap.add_argument("--zooms", default="0-7")
    ap.add_argument("--deflate-level", type=int, default=6)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    cache_dir = args.cache_dir or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".pbf_cache")
    verbose = not args.quiet
    land_shp = download_natural_earth(f"ne_{args.resolution}_land", "physical",
                                      args.resolution, cache_dir)
    border_shp = download_natural_earth(
        f"ne_{args.resolution}_admin_0_boundary_lines_land", "cultural",
        args.resolution, cache_dir)
    if verbose:
        print(f"[*] land: {land_shp}")
        print(f"[*] borders: {border_shp}")
    stats = build_basemap(load_shapes(land_shp), load_shapes(border_shp),
                          args.out, zoom_levels=_parse_zooms(args.zooms),
                          deflate_level=args.deflate_level, verbose=verbose)
    if verbose:
        print(f"[*] container: {stats['bytes'] / 1e6:.2f} MB, "
              f"zooms {sorted(stats['zooms'])}")
        print("[*] copy it to the /maps folder of the SD as basemap.mmap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
