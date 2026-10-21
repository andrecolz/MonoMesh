#!/bin/sh
# Builds the native MonoMesh tile generator.
#
#   ./tools/native/build.sh            # auto-detect compiler, static binary
#   ./tools/native/build.sh --debug    # -O0 -g build
#   CC=gcc ./tools/native/build.sh     # use another compiler
#
# A Zig toolchain (which bundles a complete C compiler and musl libc, no root
# required) is picked up automatically from tools/.toolchain if present.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$HERE/monomesh-tiles"
DEBUG=0
[ "$1" = "--debug" ] && DEBUG=1

ZIG="${ZIG:-}"
if [ -z "$ZIG" ]; then
    if command -v zig >/dev/null 2>&1; then
        ZIG=$(command -v zig)
    else
        ZIG=$(ls -d "$HERE"/../.toolchain/zig-*/zig 2>/dev/null | head -n 1 || true)
    fi
fi

ZLIB_SRC="$HERE/third_party/zlib"
SRCS="$HERE/main.c $HERE/pbf.c $HERE/cache.c $HERE/render.c $HERE/common.c $HERE/libmapcore/glyph_atlas.c"
ZLIBS="$ZLIB_SRC/adler32.c $ZLIB_SRC/compress.c $ZLIB_SRC/crc32.c $ZLIB_SRC/deflate.c \
$ZLIB_SRC/inflate.c $ZLIB_SRC/inffast.c $ZLIB_SRC/inftrees.c $ZLIB_SRC/trees.c $ZLIB_SRC/zutil.c \
$ZLIB_SRC/uncompr.c"

CFLAGS="-std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -I$HERE -I$HERE/libmapcore -I$ZLIB_SRC"
[ "$DEBUG" = "1" ] && CFLAGS="-std=gnu11 -D_GNU_SOURCE -O0 -g -Wall -Wextra -I$HERE -I$HERE/libmapcore -I$ZLIB_SRC"

echo "[*] Compilatore: ${ZIG:-${CC:-cc}}"
cd "$HERE"

if [ -n "$ZIG" ]; then
    # Keep the compiler cache inside the workspace so the build works even when
    # $HOME is not writable.
    ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$HERE/.zig-cache}"
    ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$HERE/.zig-cache}"
    export ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR
    mkdir -p "$ZIG_GLOBAL_CACHE_DIR"
    # shellcheck disable=SC2086
    "$ZIG" cc -target x86_64-linux-musl -static $CFLAGS \
        $SRCS $ZLIBS -o "$OUT" -lpthread -lm
else
    # shellcheck disable=SC2086
    ${CC:-cc} $CFLAGS $SRCS $ZLIBS -o "$OUT" -lpthread -lm
fi

echo "[+] Binario: $OUT"
ls -lh "$OUT" | awk '{print "    " $5 " " $9}'
