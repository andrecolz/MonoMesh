#!/usr/bin/env python3
"""Compose the single factory image burned at 0x0 in production.

Layout (the same one `pio run -t upload` writes):

    0x00000  bootloader.bin
    0x08000  partitions.bin
    0x10000  firmware.bin

The image header at 0x0 declares the flash mode used by the ROM to boot and it
*MUST* be DIO (byte 2 == 0x02). With QIO the ESP32-S3 ROM cannot read the flash
back: the boot stops right after the first `load:` line with `ets_loader.c 78`
and the chip watchdog-resets forever (TG0WDT_SYS_RST). That is the reason the
manufacturer rejected the v1.0b factory image. `pio run -t upload` never shows
the problem because PlatformIO normalises qio to dio in the images it creates;
only a hand-made merge stamped QIO into the header.

This script fails loudly instead of producing such an image, and checks that
the appended SHA-256 of every image is still valid and that the application
fits in the `factory` partition.

Usage:
    python3 tools/make_factory_bin.py -o dist/MonoMesh.factory.bin

Options:
    --build-dir DIR   PlatformIO build directory (default:
                      .pio/build/m5paper-mono next to this script)
"""

import argparse
import hashlib
import pathlib
import struct
import sys

IMAGE_MAGIC = 0xE9
PARTITION_MAGIC = 0x50AA
HEADER_LEN = 24
HASH_LEN = 32
ERASED = 0xFF
DIO = 0x02
FLASH_MODES = {0x00: "QIO", 0x01: "QOUT", 0x02: "DIO", 0x03: "DOUT"}

BOOTLOADER_OFFSET = 0x0
PARTITION_OFFSET = 0x8000
APP_OFFSET = 0x10000

APP_TYPE = 0x00
APP_SUBTYPE_FACTORY = 0x00


def die(message):
    sys.exit("error: %s" % message)


def parse_image(name, data):
    """Validate an ESP32 image, return (image length, flash mode, hash ok).

    The file produced by elf2image is the image followed, when `hash_appended`
    is set, by a 32-byte SHA-256 computed over everything before it (segment
    data plus the alignment padding, which is not part of the parsed segments).
    """
    if len(data) < HEADER_LEN or data[0] != IMAGE_MAGIC:
        die("%s: not an ESP32 image (magic 0x%02X)" % (name, data[0] if data else 0))

    mode = data[2]
    segments = data[1]
    pos = HEADER_LEN
    for index in range(segments):
        if pos + 8 > len(data):
            die("%s: truncated segment %d header" % (name, index))
        _addr, size = struct.unpack_from("<II", data, pos)
        pos += 8 + size
        if pos > len(data):
            die("%s: segment %d overruns the file" % (name, index))

    hash_ok = None
    if data[23] & 0x01:
        if pos > len(data) - HASH_LEN:
            die("%s: segment data overlaps the appended SHA-256" % name)
        hash_ok = hashlib.sha256(data[:-HASH_LEN]).digest() == data[-HASH_LEN:]
    return len(data), mode, hash_ok


def parse_partitions(data):
    """Return [(label, type, subtype, offset, size)] from a partition table."""
    entries = []
    for pos in range(0, len(data) - 31, 32):
        magic, ptype, subtype, offset, size = struct.unpack_from("<HBBII", data, pos)
        if magic != PARTITION_MAGIC:
            break
        label = data[pos + 12:pos + 28].split(b"\x00")[0].decode("ascii", "replace")
        entries.append((label, ptype, subtype, offset, size))
    if not entries:
        die("partition table: no valid entries")
    return entries


def pad_to(data, size, name):
    if len(data) > size:
        die("%s does not fit below 0x%X (%d bytes)" % (name, size, len(data)))
    return data + bytes([ERASED]) * (size - len(data))


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Build the 0x0 factory image")
    parser.add_argument("--build-dir", default=str(root / ".pio" / "build" / "m5paper-mono"))
    parser.add_argument("-o", "--output", default=str(root / "dist" / "MonoMesh.factory.bin"))
    args = parser.parse_args()

    build_dir = pathlib.Path(args.build_dir)
    output = pathlib.Path(args.output)

    images = {}
    for name, filename in (("bootloader", "bootloader.bin"), ("application", "firmware.bin")):
        path = build_dir / filename
        if not path.is_file():
            die("%s not found: run `pio run -e m5paper-mono` first" % path)
        images[name] = path.read_bytes()

    partitions_path = build_dir / "partitions.bin"
    if not partitions_path.is_file():
        die("%s not found: run `pio run -e m5paper-mono` first" % partitions_path)
    partitions = partitions_path.read_bytes()

    problems = []
    ends = {}
    for name in ("bootloader", "application"):
        end, mode, hash_ok = parse_image(name, images[name])
        ends[name] = end
        label = FLASH_MODES.get(mode, "unknown(0x%02X)" % mode)
        print("%-11s %8d B  mode=%-3s hash=%s" % (
            name, end, label,
            "ok" if hash_ok else ("not present" if hash_ok is None else "INVALID")))
        if mode != DIO:
            problems.append(
                "%s image header declares %s (0x%02X) at byte 2, expected DIO (0x02)"
                % (name, label, mode))
        if hash_ok is False:
            problems.append("%s: appended SHA-256 does not match the image" % name)

    entries = parse_partitions(partitions)
    print("partitions  %8d B  %d entries" % (len(partitions), len(entries)))

    factory = None
    for label, ptype, subtype, offset, size in entries:
        if ptype == APP_TYPE and subtype == APP_SUBTYPE_FACTORY:
            factory = (label, offset, size)
    if factory is None:
        problems.append("partition table has no `factory` application partition")
    else:
        label, offset, size = factory
        print("factory app `%s`: offset 0x%X size 0x%X" % (label, offset, size))
        if offset != APP_OFFSET:
            problems.append(
                "factory partition starts at 0x%X, expected 0x%X" % (offset, APP_OFFSET))
        if ends["application"] > size:
            problems.append(
                "application (%d B) does not fit in the factory partition (0x%X)"
                % (ends["application"], size))

    if problems:
        for problem in problems:
            print("error: %s" % problem, file=sys.stderr)
        die("refusing to build the factory image; the bootloader image must "
            "declare flash mode DIO (see platformio.ini)")

    image = (
        pad_to(images["bootloader"], PARTITION_OFFSET, "bootloader")
        + pad_to(partitions, APP_OFFSET - PARTITION_OFFSET, "partition table")
        + images["application"]
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)

    written = output.read_bytes()
    if written[2] != DIO:
        die("the written image has flash mode 0x%02X, expected DIO" % written[2])

    digest = hashlib.sha256(written).hexdigest()
    print("factory bin %s: %d B, header mode=DIO, sha256=%s" % (
        output, len(written), digest))
    print("flash with: esptool.py --chip esp32s3 erase_flash && "
          "esptool.py --chip esp32s3 write_flash 0x0 %s" % output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
