#!/usr/bin/env python3
"""MonoMesh packed map container (.mmap) - format v1.

MonoMesh maps are monochrome raster tiles of a fixed size (default 256x256)
covering a rectangular tile grid per zoom level.  Everything a device needs for
one region lives in a single file: no directory walk, no per-tile filesystem
metadata, no PNG/JPEG decoding on the microcontroller.

Design rules
------------
* Little endian, fixed size records, all offsets absolute from the file start.
* One section table so new payloads (POI, preview, extra layers) can be added
  without changing the reader of the existing ones.
* Per zoom: a presence bitmap + a per-row prefix so a tile lookup is O(1) with
  two small reads; the per-tile index holds only the tiles that exist.
* Tile payload is the raw monochrome bitmap (1 bit/pixel, or 2 bits/pixel for
  4 gray levels) compressed with a zlib stream.  A solid tile has no payload at
  all, the entry says which colour to fill with.
* The payload is *not* rotated: the firmware expands packed bits into its own
  composition buffer, so the container stays independent from the panel.

File layout
-----------
    header           128 bytes (see HEADER_STRUCT)
    section table    section_count * 16 bytes
    sections         in the same order as the table
    payload          per-tile zlib streams, 4-byte aligned

Header (128 bytes)::

    0   8s  magic "MONOMMAP"
    8   H   version
    10  H   header_size (128)
    12  I   flags         (see FLAG_*)
    16  I   tile_px       (256)
    20  B   payload_bits  (1 or 2)
    21  B   zoom_count
    22  H   section_count
    24  I   section_table_off
    28  I   file_size
    32  4i  region bbox in degrees * 1e7 (min_lat, min_lon, max_lat, max_lon)
    48  32s region name (UTF-8, NUL padded)
    80  48s attribution (UTF-8, NUL padded)

Section table entry (16 bytes)::

    0   I   type (SEC_*)
    4   I   offset (from file start)
    8   I   size
    12  I   aux (SEC_ZOOM: zoom level; SEC_POI: entry count;
                 SEC_PREVIEW: width << 16 | height)

SEC_ZOOM section (64 byte header + arrays)::

    0   B   zoom
    1   B   payload_bits
    2   H   flags
    4   I   x0 / 8 nx / 12 y0 / 16 ny      (grid, inclusive origin + counts)
    20  I   presence_off    nx*ny bits, row major, MSB first, 1 = tile present
    24  I   row_prefix_off  (ny+1) * u32 cumulative present count per row
    28  I   index_off       present_count * TILE_ENTRY
    32  I   payload_off     first tile payload
    36  I   payload_size    bytes occupied by the payloads
    40  I   present_count
    44  I   reserved (20 bytes, zero)

Tile entry (8 bytes)::

    0   I   payload offset
    4   H   payload length (0 when blank)
    6   H   flags (TILE_FLAG_BLANK, TILE_FLAG_BLANK_BLACK)

SEC_POI entry (20 bytes)::

    0   i   lat * 1e7
    4   i   lon * 1e7
    8   B   kind  (PLACE_KINDS index)
    9   B   reserved
    10  H   name length (bytes in SEC_STRINGS)
    12  I   population
    16  I   name offset (bytes in SEC_STRINGS)

SEC_STRINGS holds the concatenated UTF-8 names referenced by SEC_POI.
SEC_PREVIEW holds a low resolution 4-level preview (deflated, 2 bits/pixel) for
the on-device map picker; the firmware may ignore it.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile
import time
import zlib

MAGIC = b"MONOMMAP"
VERSION = 1
HEADER_SIZE = 128
SECTION_ENTRY_SIZE = 16
ZOOM_HEADER_SIZE = 64
TILE_ENTRY_SIZE = 8
POI_ENTRY_SIZE = 20

SEC_ZOOM = 1
SEC_POI = 2
SEC_STRINGS = 3
SEC_PREVIEW = 4

FLAG_DEFLATE = 1 << 0
FLAG_2BIT = 1 << 1
FLAG_PREVIEW = 1 << 2
FLAG_POI = 1 << 3
FLAG_DARK = 1 << 4

TILE_FLAG_BLANK = 1 << 0
TILE_FLAG_BLANK_BLACK = 1 << 1

ATTRIBUTION = "Map data (c) OpenStreetMap contributors (ODbL)"

HEADER_STRUCT = struct.Struct("<8sHHIIBBHII4i32s48s")
SECTION_STRUCT = struct.Struct("<IIII")
ZOOM_STRUCT = struct.Struct("<BBH" + "I" * 10 + "20x")
TILE_STRUCT = struct.Struct("<IHH")
POI_STRUCT = struct.Struct("<iiBBHII")
assert ZOOM_STRUCT.size == ZOOM_HEADER_SIZE
assert HEADER_STRUCT.size == HEADER_SIZE
assert TILE_STRUCT.size == TILE_ENTRY_SIZE
assert POI_STRUCT.size == POI_ENTRY_SIZE

# The 4x4 ordered dither used by the SSD1677 driver when it maps an 8-bit grey
# value onto the four levels it can display.  Baking the same pattern into the
# container keeps the anti-aliased labels identical to the old 8-bit PNG path.
BAYER4 = (-30, 18, -22, 26, -14, 2, -6, 10, -18, 30, -26, 22, -2, 14, -10, 6)

# 8-bit values that map to exactly one of the four panel levels (no dithering on
# the device): level = value >> 6 for these values whichever Bayer offset is
# added by the driver.
LEVEL8 = (0, 96, 160, 255)

PLACE_KINDS = ["city", "town", "village", "suburb", "hamlet", "neighbourhood"]


# --------------------------------------------------------------------------- #
# Bit packing helpers
# --------------------------------------------------------------------------- #
_SPREAD_MASK_CACHE: dict = {}


def _spread_masks(nbits: int):
    """(shift, mask) pairs that spread nbits into 2*nbits with zero gaps.

    Built once per tile size: the masks are pure overhead otherwise (a single
    tile would otherwise pay ~1e6 Python loop iterations to build them).
    """
    masks = _SPREAD_MASK_CACHE.get(nbits)
    if masks is None:
        full = (1 << (2 * nbits)) - 1
        masks = []
        w = nbits >> 1
        while w:
            unit = (1 << w) - 1
            m = unit
            total = 2 * w
            while total < 2 * nbits:
                m |= m << total
                total *= 2
            masks.append((w, m & full))
            w >>= 1
        _SPREAD_MASK_CACHE[nbits] = masks
    return masks


def _spread_bits(x: int, nbits: int) -> int:
    """Spread ``nbits`` bits so that input bit p lands on output bit 2*p."""
    for w, mask in _spread_masks(nbits):
        x = (x | (x << w)) & mask
    return x


def _spread_bits_reference(x: int, nbits: int) -> int:
    """Naive reference implementation, used by the self-test."""
    out = 0
    for p in range(nbits):
        if (x >> p) & 1:
            out |= 1 << (2 * p)
    return out


def _pil_module():
    try:
        from PIL import Image, ImageChops  # noqa: F401
    except ImportError as err:  # pragma: no cover
        raise SystemExit(f"[!] Pillow richiesto per gestire i tile: {err}")
    return Image, ImageChops


def bayer_offsets(ref_px: int):
    """The 16 Bayer offsets in tile-local (x, y) order, in panel coordinates.

    The SSD1677 driver indexes the table as ``Bayer[(y_native & 3) * 4 + (x_native & 3)]``
    and on this board the portrait screen (x, y) maps to native x = y and
    native y = ref_px - 1 - x, so the pattern seen in tile coordinates is that
    rotated/flipped version of the table.
    """
    offs = []
    for y in range(4):
        for x in range(4):
            native_x = y
            native_y = (ref_px - 1 - x) & 3
            offs.append(BAYER4[(native_y & 3) * 4 + (native_x & 3)])
    return offs


_OFFSETS_CACHE: dict = {}


def _bayer_threshold_image(tile_px: int, threshold: int, ref_px: int, Image):
    """Tiled image of ``threshold - offset - 1`` so that a saturating subtract
    behaves exactly like the driver's ``(value + offset) >> 6`` comparison.

    The 4x4 cell must *repeat* every four pixels (that is how the driver scans
    the panel), so the image is built row by row instead of being resized.
    """
    offs = _OFFSETS_CACHE.get(ref_px)
    if offs is None:
        offs = bayer_offsets(ref_px)
        _OFFSETS_CACHE[ref_px] = offs
    cells = [max(0, min(255, threshold - offs[y * 4 + x] - 1)) for y in range(4) for x in range(4)]
    rows = [bytes(cells[(y & 3) * 4 + (x & 3)] for x in range(tile_px)) for y in range(4)]
    data = b"".join(rows[y & 3] for y in range(tile_px))
    return Image.frombytes("L", (tile_px, tile_px), data)


_POSITIVE_LUT = bytes([0] + [255] * 255)


def reference_levels(im, ref_px: int) -> bytes:
    """Naive per-pixel 4-level quantisation, used to test the fast path."""
    offs = bayer_offsets(ref_px)
    data = im.tobytes()
    w, h = im.size
    out = bytearray(len(data))
    for y in range(h):
        row = y * w
        for x in range(w):
            v = data[row + x] + offs[(y & 3) * 4 + (x & 3)]
            out[row + x] = 0 if v < 64 else 1 if v < 128 else 2 if v < 192 else 3
    return bytes(out)


_PATTERN_CACHE: dict = {}


def pack_tile_image(im, tile_px: int = 256, bits: int = 1, dither: bool = True,
                    ref_px: int = 480) -> bytes:
    """Packs a PIL "L" image into the container payload format.

    ``bits == 1``: plain threshold at 128 (same as the old ``--png-mode mono``).
    ``bits == 2``: four levels.  With ``dither`` the 4x4 Bayer pattern of the
    panel is applied first, so the result matches what the old 8-bit PNGs looked
    like on the device; without it the image is a plain 4-level quantisation.
    """
    Image, ImageChops = _pil_module()
    if im.mode != "L":
        im = im.convert("L")
    if bits == 1:
        return im.point(lambda v: 255 if v >= 128 else 0).convert(
            "1", dither=Image.Dither.NONE).tobytes()
    if bits != 2:
        raise ValueError("bits must be 1 or 2")

    if dither:
        # Three level thresholds, each with the position dependent Bayer offset
        # folded in: level >= k  <=>  value >= (64*k - offset).  A saturating
        # subtract keeps everything in C speed, no per-pixel Python.
        masks = []
        for k in (1, 2, 3):
            key = (tile_px, ref_px, k)
            thr = _PATTERN_CACHE.get(key)
            if thr is None:
                thr = _bayer_threshold_image(tile_px, 64 * k, ref_px, Image)
                _PATTERN_CACHE[key] = thr
            masks.append(ImageChops.subtract(im, thr).point(_POSITIVE_LUT).convert(
                "1", dither=Image.Dither.NONE))
        m1, m2, m3 = masks
        plane_a = m2                       # level bit 1 (values 2 and 3)
        plane_b = ImageChops.logical_xor(ImageChops.logical_xor(m1, m2), m3)
    else:
        plane_a = im.point(lambda v: 255 if v >= 128 else 0).convert("1", dither=Image.Dither.NONE)
        plane_b = im.point(lambda v: 255 if (64 <= v < 128) or v >= 192 else 0).convert(
            "1", dither=Image.Dither.NONE)

    bitmap = _interleave_planes(plane_a.tobytes(), plane_b.tobytes(), tile_px * tile_px)
    return bitmap


def _interleave_planes(plane_a: bytes, plane_b: bytes, nbits: int) -> bytes:
    a = int.from_bytes(plane_a, "big")
    b = int.from_bytes(plane_b, "big")
    out = (_spread_bits(a, nbits) << 1) | _spread_bits(b, nbits)
    return out.to_bytes((nbits * 2 + 7) // 8, "big")


def unpack_tile(data: bytes, tile_px: int, bits: int) -> bytes:
    """Unpacks a payload into 8-bit grey values (for verify/dump/info)."""
    nbits = tile_px * tile_px
    if bits == 1:
        out = bytearray(nbits)
        for i in range(nbits):
            out[i] = 255 if (data[i >> 3] >> (7 - (i & 7))) & 1 else 0
        return bytes(out)
    out = bytearray(nbits)
    for i in range(nbits):
        shift = 6 - 2 * (i & 3)
        v = (data[i >> 2] >> shift) & 3
        out[i] = LEVEL8[v]
    return bytes(out)


def blank_kind(payload: bytes) -> int:
    """0 = has content, 1 = solid white, 2 = solid black."""
    if not payload.strip(b"\xff"):
        return 1
    if not payload.strip(b"\x00"):
        return 2
    return 0


# --------------------------------------------------------------------------- #
# Writer
# --------------------------------------------------------------------------- #
class MapWriter:
    """Streams tile payloads to a temporary file and writes the container at close()."""

    def __init__(self, path: str, *, bits: int = 1, tile_px: int = 256, name: str = "",
                 attribution: str = ATTRIBUTION, bbox=None, dark: bool = False,
                 deflate_level: int = 6, ref_px: int = 480, progress=None):
        if bits not in (1, 2):
            raise ValueError("bits must be 1 or 2")
        if bbox is None:
            raise ValueError("bbox is required")
        self.path = path
        self.bits = bits
        self.tile_px = tile_px
        self.name = name
        self.attribution = attribution
        self.bbox = tuple(float(v) for v in bbox)
        self.dark = bool(dark)
        self.deflate_level = int(deflate_level)
        self.ref_px = ref_px
        self.progress = progress

        # z -> {(x, y): (offset_in_temp, length, flags)}
        self._entries: dict[int, dict[tuple[int, int], tuple[int, int, int]]] = {}
        self._payload_size = 0
        self._tiles = 0
        self._blank = 0
        self._raw_bytes = 0
        self._pois: list = []
        self._strings = bytearray()
        self._preview = None  # (w, h, packed bytes)
        self._tmp = tempfile.NamedTemporaryFile(
            "wb", delete=False, dir=os.path.dirname(os.path.abspath(path)) or ".",
            prefix=".mmap-payload-", suffix=".tmp")

    # ---- tiles ----
    def add_tile_compressed(self, z: int, x: int, y: int, data: bytes,
                            blank: int = 0) -> int:
        """Adds an already packed and deflated tile. Returns the stored length."""
        flags = 0
        if blank:
            flags = TILE_FLAG_BLANK | (TILE_FLAG_BLANK_BLACK if blank == 2 else 0)
            data = b""
            self._blank += 1
        if data:
            self._tmp.write(data)
        entry = (self._payload_size, len(data), flags)
        self._payload_size += len(data)
        self._entries.setdefault(z, {})[(x, y)] = entry
        self._tiles += 1
        return len(data)

    def add_tile_bits(self, z: int, x: int, y: int, payload: bytes,
                      blank: int = 0) -> int:
        """Adds a packed (uncompressed) tile: compresses it here."""
        if blank or not payload:
            return self.add_tile_compressed(z, x, y, b"", blank)
        return self.add_tile_compressed(z, x, y, zlib.compress(payload, self.deflate_level))

    def add_tile_image(self, z: int, x: int, y: int, im) -> int:
        packed = pack_tile_image(im, self.tile_px, self.bits, dither=self._dither_for(),
                                 ref_px=self.ref_px)
        self._raw_bytes += len(packed)
        return self.add_tile_bits(z, x, y, packed, blank=blank_kind(packed))

    def _dither_for(self) -> bool:
        # 1-bit tiles are thresholded: dithering them only adds speckle.
        return self.bits == 2

    # ---- extra sections ----
    def add_poi(self, lat: float, lon: float, kind: str, population: int = 0, name: str = ""):
        idx = PLACE_KINDS.index(kind) if kind in PLACE_KINDS else 255
        off = len(self._strings)
        raw = name.encode("utf-8")[:65535]
        self._strings += raw
        self._pois.append((int(round(lat * 1e7)), int(round(lon * 1e7)), idx, 0,
                           len(raw), int(population or 0), off))

    def set_preview(self, im, bits: int = 2):
        Image, _ = _pil_module()
        if im.mode != "L":
            im = im.convert("L")
        packed = pack_tile_image(im, im.size[0], 2, dither=True, ref_px=self.ref_px)
        self._preview = (im.size[0], im.size[1], zlib.compress(packed, self.deflate_level))

    # ---- finalise ----
    def close(self) -> dict:
        zooms = sorted(self._entries)
        tiles_per_zoom = {z: len(self._entries[z]) for z in zooms}

        # Layout first: the absolute payload offsets written into the index depend
        # on the total size of the sections.
        plans = []
        for z in zooms:
            x0, y0, nx, ny = self._grid_bbox(z)
            presence_bytes = (nx * ny + 7) // 8
            prefix_bytes = (ny + 1) * 4
            index_bytes = tiles_per_zoom[z] * TILE_ENTRY_SIZE
            plans.append((z, x0, y0, nx, ny, presence_bytes, prefix_bytes, index_bytes))

        n_sections = len(plans) + (2 if self._pois else 0) + (1 if self._preview else 0)
        cursor = HEADER_SIZE + n_sections * SECTION_ENTRY_SIZE
        zoom_off = {}
        for (z, _x0, _y0, _nx, _ny, pb, fb, ib) in plans:
            zoom_off[z] = cursor
            cursor += ZOOM_HEADER_SIZE + pb + fb + ib
        poi_off = strings_off = preview_off = 0
        if self._pois:
            poi_off = cursor
            cursor += 4 + POI_ENTRY_SIZE * len(self._pois)
            strings_off = cursor
            cursor += len(self._strings)
        if self._preview:
            preview_off = cursor
            cursor += 4 + len(self._preview[2])
        payload_start = (cursor + 3) & ~3

        sections = []  # (type, aux, offset, blob)
        for (z, x0, y0, nx, ny, pb, fb, ib) in plans:
            sec = bytearray(ZOOM_HEADER_SIZE + pb + fb + ib)
            presence_off = ZOOM_HEADER_SIZE
            prefix_off = presence_off + pb
            index_off = prefix_off + fb
            entries = self._entries[z]
            rank = 0
            prefix = []
            for yy in range(ny):
                prefix.append(rank)
                for xx in range(nx):
                    entry = entries.get((x0 + xx, y0 + yy))
                    if entry is None:
                        continue
                    bit = yy * nx + xx
                    sec[presence_off + (bit >> 3)] |= 0x80 >> (bit & 7)
                    off, length, tile_flags = entry
                    struct.pack_into("<IHH", sec, index_off + rank * TILE_ENTRY_SIZE,
                                     (payload_start + off) if length else 0, length, tile_flags)
                    rank += 1
            prefix.append(rank)
            for i, v in enumerate(prefix):
                struct.pack_into("<I", sec, prefix_off + i * 4, v)
            payload_bytes = sum(e[1] for e in entries.values())
            ZOOM_STRUCT.pack_into(sec, 0, z, self.bits, FLAG_DEFLATE,
                                  x0, nx, y0, ny, presence_off, prefix_off, index_off,
                                  payload_start, payload_bytes, rank)
            sections.append((SEC_ZOOM, z, zoom_off[z], bytes(sec)))

        if self._pois:
            blob = bytearray(4 + POI_ENTRY_SIZE * len(self._pois))
            struct.pack_into("<I", blob, 0, len(self._pois))
            for i, entry in enumerate(self._pois):
                POI_STRUCT.pack_into(blob, 4 + i * POI_ENTRY_SIZE, *entry)
            sections.append((SEC_POI, len(self._pois), poi_off, bytes(blob)))
            sections.append((SEC_STRINGS, len(self._strings), strings_off, bytes(self._strings)))
        if self._preview:
            w, h, blob = self._preview
            sections.append((SEC_PREVIEW, (w << 16) | h, preview_off,
                             struct.pack("<HH", w, h) + blob))

        flags = FLAG_DEFLATE
        if self.bits == 2:
            flags |= FLAG_2BIT
        if self._pois:
            flags |= FLAG_POI
        if self._preview:
            flags |= FLAG_PREVIEW
        if self.dark:
            flags |= FLAG_DARK

        file_size = payload_start + self._payload_size
        header = HEADER_STRUCT.pack(
            MAGIC, VERSION, HEADER_SIZE, flags, self.tile_px, self.bits,
            len(zooms), len(sections), HEADER_SIZE, file_size,
            int(round(self.bbox[0] * 1e7)), int(round(self.bbox[1] * 1e7)),
            int(round(self.bbox[2] * 1e7)), int(round(self.bbox[3] * 1e7)),
            self.name.encode("utf-8")[:31], self.attribution.encode("utf-8")[:47])
        assert len(header) == HEADER_SIZE

        with open(self.path, "wb") as out:
            out.write(header)
            for sec_type, aux, off, blob in sections:
                out.write(SECTION_STRUCT.pack(sec_type, off, len(blob), aux))
            for sec_type, aux, off, blob in sections:
                assert out.tell() == off, (out.tell(), off)
                out.write(blob)
            out.seek(payload_start)
            self._tmp.flush()
            self._tmp.close()
            with open(self._tmp.name, "rb") as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
            out.truncate(file_size)
        os.unlink(self._tmp.name)

        return {
            "path": self.path,
            "bytes": file_size,
            "tiles": self._tiles,
            "blank": self._blank,
            "payload_bytes": self._payload_size,
            "raw_bytes": self._raw_bytes,
            "zooms": tiles_per_zoom,
            "pois": len(self._pois),
        }

    def _grid_bbox(self, z: int):
        xs = [k[0] for k in self._entries[z]]
        ys = [k[1] for k in self._entries[z]]
        x0, x1 = min(xs), max(xs)
        y0, y1 = min(ys), max(ys)
        return (x0, y0, x1 - x0 + 1, y1 - y0 + 1)

    def abort(self):
        try:
            self._tmp.close()
            os.unlink(self._tmp.name)
        except (OSError, AttributeError):
            pass

    def __del__(self):
        # A run that dies before close() must not leave a payload temp behind
        # (dead runs have left multi-hundred-MB files next to the .mmap).
        self.abort()


# --------------------------------------------------------------------------- #
# Reader (verification / tooling)
# --------------------------------------------------------------------------- #
class MapReader:
    def __init__(self, path: str):
        self.path = path
        self.file_size = os.path.getsize(path)
        with open(path, "rb") as fh:
            self.header = HEADER_STRUCT.unpack(fh.read(HEADER_SIZE))
            if self.header[0] != MAGIC:
                raise ValueError(f"not a MonoMesh map: {path}")
            (_, self.version, self.header_size, self.flags, self.tile_px, self.bits,
             self.zoom_count, self.section_count, self.table_off, self.declared_size,
             *rest) = self.header
            bbox = rest[:4]
            self.bbox = tuple(v / 1e7 for v in bbox)
            self.name = rest[4].split(b"\x00")[0].decode("utf-8", "replace")
            self.attribution = rest[5].split(b"\x00")[0].decode("utf-8", "replace")
            fh.seek(self.table_off)
            raw = fh.read(SECTION_ENTRY_SIZE * self.section_count)
        self.sections = []
        for i in range(self.section_count):
            sec_type, off, size, aux = SECTION_STRUCT.unpack_from(raw, i * SECTION_ENTRY_SIZE)
            self.sections.append((sec_type, off, size, aux))
        self.zooms = {}
        for sec_type, off, size, aux in self.sections:
            if sec_type == SEC_ZOOM:
                self.zooms[aux] = self._read_zoom(off)
        self.pois = []
        self._read_pois()

    def _read_zoom(self, off):
        with open(self.path, "rb") as fh:
            fh.seek(off)
            head = fh.read(ZOOM_HEADER_SIZE)
            (z, bits, flags, x0, nx, y0, ny, presence_off, prefix_off, index_off,
             payload_off, payload_size, present) = ZOOM_STRUCT.unpack(head)
            presence_bytes = (nx * ny + 7) // 8
            fh.seek(off + presence_off)
            presence = fh.read(presence_bytes)
            fh.seek(off + prefix_off)
            prefix = struct.unpack(f"<{ny + 1}I", fh.read((ny + 1) * 4))
            fh.seek(off + index_off)
            index = fh.read(present * TILE_ENTRY_SIZE)
        return {"z": z, "bits": bits, "flags": flags, "x0": x0, "nx": nx, "y0": y0,
                "ny": ny, "presence": presence, "prefix": prefix, "index": index,
                "payload_off": payload_off, "payload_size": payload_size,
                "present": present}

    def _read_pois(self):
        strings = b""
        poi_sec = None
        for sec_type, off, size, aux in self.sections:
            if sec_type == SEC_STRINGS:
                with open(self.path, "rb") as fh:
                    fh.seek(off)
                    strings = fh.read(size)
            elif sec_type == SEC_POI:
                poi_sec = (off, size)
        if not poi_sec:
            return
        off, size = poi_sec
        with open(self.path, "rb") as fh:
            fh.seek(off)
            raw = fh.read(size)
        count = struct.unpack_from("<I", raw, 0)[0]
        for i in range(count):
            lat, lon, kind, _res, name_len, pop, name_off = POI_STRUCT.unpack_from(
                raw, 4 + i * POI_ENTRY_SIZE)
            name = strings[name_off:name_off + name_len].decode("utf-8", "replace")
            self.pois.append({"lat": lat / 1e7, "lon": lon / 1e7,
                              "kind": PLACE_KINDS[kind] if kind < len(PLACE_KINDS) else "other",
                              "population": pop, "name": name})

    def tile(self, z: int, x: int, y: int):
        """Returns (payload or None, flags). None = blank tile."""
        info = self.zooms.get(z)
        if not info:
            return None, 0
        if not (info["x0"] <= x < info["x0"] + info["nx"]):
            return None, 0
        if not (info["y0"] <= y < info["y0"] + info["ny"]):
            return None, 0
        bit = (y - info["y0"]) * info["nx"] + (x - info["x0"])
        if not (info["presence"][bit >> 3] >> (7 - (bit & 7))) & 1:
            return None, 0
        # rank = present tiles before this one in row major order
        row = y - info["y0"]
        rank = info["prefix"][row]
        row_start = row * info["nx"]
        for i in range(row_start, bit):
            if (info["presence"][i >> 3] >> (7 - (i & 7))) & 1:
                rank += 1
        off, length, flags = TILE_STRUCT.unpack_from(info["index"], rank * TILE_ENTRY_SIZE)
        if flags & TILE_FLAG_BLANK:
            return None, flags
        with open(self.path, "rb") as fh:
            fh.seek(off)
            return zlib.decompress(fh.read(length)), flags

    def stats(self) -> dict:
        per_zoom = {}
        for z, info in sorted(self.zooms.items()):
            per_zoom[z] = {"tiles": info["present"], "grid": (info["nx"], info["ny"]),
                           "payload_bytes": info["payload_size"]}
        return per_zoom


# --------------------------------------------------------------------------- #
# CLI: info / dump / verify / selftest
# --------------------------------------------------------------------------- #
def _cmd_info(args) -> int:
    r = MapReader(args.file)
    print(f"file:        {args.file} ({r.file_size:,} bytes)")
    print(f"version:     {r.version}  bits/tile: {r.bits}  tile: {r.tile_px}px")
    print(f"region:      {r.name!r}  bbox: {r.bbox}")
    print(f"attribution: {r.attribution}")
    print(f"zooms:       {sorted(r.zooms)}  sections: {len(r.sections)}  poi: {len(r.pois)}")
    total = 0
    print(f"{'z':>3} {'tiles':>8} {'grid':>12} {'payload':>12} {'avg':>9}")
    for z, info in sorted(r.zooms.items()):
        avg = info["payload_size"] / max(1, info["present"])
        total += info["payload_size"]
        grid = f"{info['nx']}x{info['ny']}"
        print(f"{z:>3} {info['present']:>8} {grid:>12} {info['payload_size']:>12,} {avg:>9.0f}")
    print(f"payload total: {total:,} bytes")
    return 0


def _cmd_dump(args) -> int:
    Image, _ = _pil_module()
    r = MapReader(args.file)
    payload, flags = r.tile(args.z, args.x, args.y)
    if payload is None:
        blank = 255 if not (flags & TILE_FLAG_BLANK_BLACK) else 0
        img = Image.new("L", (r.tile_px, r.tile_px), blank)
    else:
        img = Image.frombytes("L", (r.tile_px, r.tile_px), unpack_tile(payload, r.tile_px, r.bits))
    img.save(args.out)
    print(f"wrote {args.out} ({r.tile_px}x{r.tile_px}, {r.bits} bit/pixel)")
    return 0


def _cmd_verify(args) -> int:
    """Decodes tiles and, when a PNG tree is given, compares them pixel by pixel."""
    import glob
    import random
    Image, _ = _pil_module()
    r = MapReader(args.file)
    problems = []
    checked = 0
    if args.tiles_dir:
        files = glob.glob(os.path.join(args.tiles_dir, "*", "*", "*.png"))
        random.seed(1234)
        random.shuffle(files)
        for f in files[:args.sample]:
            parts = f.split(os.sep)
            z = int(parts[-3])
            x = int(parts[-2])
            y = int(parts[-1][:-4])
            payload, flags = r.tile(z, x, y)
            if payload is None:
                if not (flags & TILE_FLAG_BLANK):
                    problems.append(f"{z}/{x}/{y}: present in the index but empty")
                continue
            got = unpack_tile(payload, r.tile_px, r.bits)
            im = Image.open(f)
            if im.mode != "L":
                im = im.convert("L")
            want = pack_tile_image(im, r.tile_px, r.bits, dither=True)
            if want != payload:
                problems.append(f"{z}/{x}/{y}: payload differs from the render")
            checked += 1
    # structural checks
    for sec_type, off, size, aux in r.sections:
        if off < HEADER_SIZE or off + size > r.file_size:
            problems.append(f"section {sec_type} out of file bounds ({off}+{size})")
    for z, info in r.zooms.items():
        if not (info["prefix"][0] == 0):
            problems.append(f"z{z}: row_prefix[0] != 0")
        if info["prefix"][-1] != info["present"]:
            problems.append(f"z{z}: row_prefix[-1] != present_count")
    print(f"verify: {len(r.zooms)} zoom levels, {sum(i['present'] for i in r.zooms.values())} indexed tiles, "
          f"{len(r.pois)} POIs, {checked} tiles compared against PNGs")
    if problems:
        print(f"[!] {len(problems)} problems:")
        for p in problems[:20]:
            print("   ", p)
        return 1
    print("[OK] structure and payload are consistent")
    return 0


def _cmd_selftest(_args) -> int:
    import random
    random.seed(7)
    Image, _ = _pil_module()
    # bit spreading vs the naive reference
    for nbits in (8, 16, 64, 256, 1024):
        for _ in range(20):
            v = random.getrandbits(nbits)
            if _spread_bits(v, nbits) != _spread_bits_reference(v, nbits):
                print(f"[!] spread mismatch nbits={nbits}")
                return 1
    # packing round trip
    for bits in (1, 2):
        for trial in range(20):
            im = Image.new("L", (64, 64))
            px = im.load()
            for y in range(64):
                for x in range(64):
                    px[x, y] = random.randrange(4) * 85
            packed = pack_tile_image(im, 64, bits, dither=(bits == 2), ref_px=480)
            expect_len = (64 * 64 * bits + 7) // 8
            if len(packed) != expect_len:
                print(f"[!] payload of {len(packed)} bytes, expected {expect_len} (bits={bits})")
                return 1
            raw = unpack_tile(packed, 64, bits)
            if bits == 1:
                want = bytes(255 if v >= 128 else 0 for v in im.tobytes())
                if raw != want:
                    print(f"[!] pack/unpack mismatch bits=1 trial={trial}")
                    return 1
            else:
                # the fast path must reproduce the naive driver formula exactly
                got_levels = bytes(v >> 6 for v in raw)
                want_levels = reference_levels(im, 480)
                if got_levels != want_levels:
                    print(f"[!] 4-gray levels differ from the reference (trial={trial})")
                    for i, (a, b) in enumerate(zip(got_levels, want_levels)):
                        if a != b:
                            print(f"    first differing pixel: {i} -> {a} != {b}")
                            break
                    return 1
    print("[OK] packing/format self-test passed")
    return 0


# --------------------------------------------------------------------------- #
# pack-tree: PNG tree -> container (also the full scale benchmark)
# --------------------------------------------------------------------------- #
def _pack_one(task):
    path, z, x, y, bits, level, ref_px, dither = task
    Image, _ = _pil_module()
    t0 = time.perf_counter()
    im = Image.open(path)
    if im.mode != "L":
        im = im.convert("L")
    t1 = time.perf_counter()
    raw = pack_tile_image(im, im.size[0], bits, dither=dither, ref_px=ref_px)
    blank = blank_kind(raw)
    payload = b"" if blank else zlib.compress(raw, level)
    t2 = time.perf_counter()
    return (z, x, y, payload, blank, im.size[0], (t1 - t0) * 1000.0, (t2 - t1) * 1000.0)


def _tiles_bbox(tiles):
    """Geographic bbox from the tile coordinates (union over the zoom levels)."""
    import math
    per_zoom = {}
    for z, x, y in tiles:
        cur = per_zoom.get(z)
        if cur is None:
            per_zoom[z] = [x, x, y, y]
        else:
            cur[0] = min(cur[0], x)
            cur[1] = max(cur[1], x)
            cur[2] = min(cur[2], y)
            cur[3] = max(cur[3], y)
    lats, lons = [], []
    for z, (x0, x1, y0, y1) in per_zoom.items():
        n = 2.0 ** z
        for tx, ty in ((x0, y0), (x1 + 1, y1 + 1)):
            lon = tx / n * 360.0 - 180.0
            lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * ty / n))))
            lats.append(lat)
            lons.append(lon)
    return (min(lats), min(lons), max(lats), max(lons))


def _cmd_pack_tree(args) -> int:
    import glob
    import multiprocessing as mp
    import time
    root = args.tiles_dir
    files = glob.glob(os.path.join(root, "*", "*", "*.png"))
    if not files:
        print(f"[!] no PNG files in {root}")
        return 1
    tasks = []
    for f in files:
        parts = f.split(os.sep)
        try:
            z = int(parts[-3])
            x = int(parts[-2])
            y = int(parts[-1][:-4])
        except ValueError:
            continue
        tasks.append((f, z, x, y, args.bits, args.deflate_level, args.ref_px,
                      args.bits == 2 and not args.no_dither))
    print(f"[*] {len(tasks)} tiles from {root} -> {args.out} ({args.bits} bit/pixel, zlib {args.deflate_level})")

    bbox = _tiles_bbox([(t[1], t[2], t[3]) for t in tasks]) if args.bbox is None else args.bbox
    name = args.name or os.path.splitext(os.path.basename(args.out))[0]
    Image, _ = _pil_module()
    with Image.open(tasks[0][0]) as probe:
        tile_px = probe.size[0]
    writer = MapWriter(args.out, bits=args.bits, tile_px=tile_px, name=name, bbox=bbox,
                       deflate_level=args.deflate_level, ref_px=args.ref_px)

    jobs = args.jobs or max(1, (os.cpu_count() or 2) - 1)
    t_start = time.perf_counter()
    load_ms = pack_ms = 0.0
    done = 0
    try:
        ctx = mp.get_context("fork")
    except ValueError:  # pragma: no cover
        ctx = mp.get_context()
    with ctx.Pool(processes=jobs) as pool:
        for z, x, y, payload, blank, size, load, pack in pool.imap_unordered(
                _pack_one, tasks, chunksize=16):
            writer.add_tile_compressed(z, x, y, payload, blank)
            load_ms += load
            pack_ms += pack
            done += 1
            if args.verbose and done % 5000 == 0:
                print(f"    {done}/{len(tasks)} tile ({time.perf_counter() - t_start:.1f}s)")
    t_write = time.perf_counter()
    stats = writer.close()
    write_s = time.perf_counter() - t_write
    total_s = time.perf_counter() - t_start
    print(f"[OK] {stats['tiles']} tiles, {stats['blank']} blank, "
          f"{stats['bytes'] / 1e6:.2f} MB in {total_s:.2f}s "
          f"({stats['tiles'] / max(0.01, total_s):.0f} tile/s, {jobs} processes)")
    print(f"    PNG read      (sum)   {load_ms / 1000:.2f} s ({load_ms / max(1, done):.2f} ms/tile)")
    print(f"    pack+zlib     (sum)   {pack_ms / 1000:.2f} s ({pack_ms / max(1, done):.2f} ms/tile)")
    print(f"    container write       {write_s:.2f} s ({stats['bytes'] / max(0.001, write_s) / 1e6:.1f} MB/s)")
    print(f"    compressed payload    {stats['payload_bytes'] / 1e6:.2f} MB "
          f"({stats['payload_bytes'] / max(1, done):.0f} B/tile)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="MonoMesh .mmap container tools")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="show header and statistics")
    p.add_argument("file")
    p.set_defaults(func=_cmd_info)

    p = sub.add_parser("dump", help="export a tile as PNG")
    p.add_argument("file")
    p.add_argument("z", type=int)
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)
    p.add_argument("out")
    p.set_defaults(func=_cmd_dump)

    p = sub.add_parser("verify", help="check structure and payload")
    p.add_argument("file")
    p.add_argument("--tiles-dir", default="")
    p.add_argument("--sample", type=int, default=200)
    p.set_defaults(func=_cmd_verify)

    p = sub.add_parser("selftest", help="internal packing and format tests")
    p.set_defaults(func=_cmd_selftest)

    p = sub.add_parser("pack-tree", help="convert a PNG tree into a .mmap container")
    p.add_argument("--tiles-dir", default="./tiles")
    p.add_argument("--out", required=True)
    p.add_argument("--bits", type=int, choices=[1, 2], default=1)
    p.add_argument("--deflate-level", type=int, default=6)
    p.add_argument("--ref-px", type=int, default=480)
    p.add_argument("--no-dither", action="store_true")
    p.add_argument("--bbox", type=float, nargs=4, default=None,
                   metavar=("MIN_LAT", "MIN_LON", "MAX_LAT", "MAX_LON"))
    p.add_argument("--name", default=None)
    p.add_argument("--jobs", type=int, default=None)
    p.add_argument("--verbose", action="store_true")
    p.set_defaults(func=_cmd_pack_tree)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
