#!/bin/sh
# Builds the libmapcore host test driver (mm_host).
#   ./tools/native/build_mm_host.sh           optimized build
#   ./tools/native/build_mm_host.sh --ubsan   UBSan build for the fuzzers
# Note: AddressSanitizer is not available with the bundled Zig toolchain (its
# compiler-rt does not ship the ASan runtime); UBSan links fine and catches
# exactly what the fixed point property tests need (signed overflow, bad
# shifts, out of range casts).  Crash safety is additionally covered by plain
# signal detection in the fuzz tests.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
UBSAN=0
[ "$1" = "--ubsan" ] && UBSAN=1

ZIG="${ZIG:-}"
if [ -z "$ZIG" ]; then
    if command -v zig >/dev/null 2>&1; then
        ZIG=$(command -v zig)
    else
        ZIG=$(ls -d "$HERE"/../.toolchain/zig-*/zig 2>/dev/null | head -n 1 || true)
    fi
fi

ZLIB_SRC="$HERE/third_party/zlib"
CORE="$HERE/libmapcore"
SRCS="$HERE/mm_host.c $HERE/cache.c $HERE/common.c $HERE/render.c $HERE/libmapcore/glyph_atlas.c \
$CORE/mm_geom.c $CORE/mm_style.c $CORE/mm_raster.c $CORE/mm_text.c $CORE/mm_vmap.c \
$CORE/mm_inflate.c $CORE/mm_render.c"
ZLIBS="$ZLIB_SRC/adler32.c $ZLIB_SRC/compress.c $ZLIB_SRC/crc32.c $ZLIB_SRC/deflate.c \
$ZLIB_SRC/inflate.c $ZLIB_SRC/inffast.c $ZLIB_SRC/inftrees.c $ZLIB_SRC/trees.c $ZLIB_SRC/zutil.c \
$ZLIB_SRC/uncompr.c"

OUT="$HERE/mm_host"
CFLAGS="-std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -I$HERE -I$CORE -I$ZLIB_SRC"
if [ "$UBSAN" = "1" ]; then
    OUT="$HERE/mm_host_ubsan"
    CFLAGS="-std=gnu11 -D_GNU_SOURCE -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
-fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -I$HERE -I$CORE -I$ZLIB_SRC"
fi

if [ -n "$ZIG" ]; then
    ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$HERE/.zig-cache}"
    ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$HERE/.zig-cache}"
    export ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR
    mkdir -p "$ZIG_GLOBAL_CACHE_DIR"
    if [ "$UBSAN" = "1" ]; then
        # shellcheck disable=SC2086
        "$ZIG" cc $CFLAGS $SRCS $ZLIBS -o "$OUT" -lpthread -lm
    else
        # shellcheck disable=SC2086
        "$ZIG" cc -target x86_64-linux-musl -static $CFLAGS $SRCS $ZLIBS -o "$OUT" -lpthread -lm
    fi
else
    # shellcheck disable=SC2086
    ${CC:-cc} $CFLAGS $SRCS $ZLIBS -o "$OUT" -lpthread -lm
fi

echo "[+] Binario: $OUT"
