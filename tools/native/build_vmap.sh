#!/bin/sh
# Builds the MonoMesh vector container tool (monomesh-vmap).
# Same compiler selection as build.sh.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)

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
SRCS="$HERE/vmap_tool.c $HERE/cache.c $HERE/common.c \
$CORE/mm_geom.c $CORE/mm_style.c $CORE/mm_vmap.c $CORE/mm_inflate.c"
ZLIBS="$ZLIB_SRC/adler32.c $ZLIB_SRC/compress.c $ZLIB_SRC/crc32.c $ZLIB_SRC/deflate.c \
$ZLIB_SRC/inflate.c $ZLIB_SRC/inffast.c $ZLIB_SRC/inftrees.c $ZLIB_SRC/trees.c $ZLIB_SRC/zutil.c \
$ZLIB_SRC/uncompr.c"

CFLAGS="-std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -I$HERE -I$CORE -I$ZLIB_SRC"

if [ -n "$ZIG" ]; then
    ZIG_GLOBAL_CACHE_DIR="${ZIG_GLOBAL_CACHE_DIR:-$HERE/.zig-cache}"
    ZIG_LOCAL_CACHE_DIR="${ZIG_LOCAL_CACHE_DIR:-$HERE/.zig-cache}"
    export ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR
    mkdir -p "$ZIG_GLOBAL_CACHE_DIR"
    # shellcheck disable=SC2086
    "$ZIG" cc -target x86_64-linux-musl -static $CFLAGS \
        $SRCS $ZLIBS -o "$HERE/monomesh-vmap" -lpthread -lm
else
    # shellcheck disable=SC2086
    ${CC:-cc} $CFLAGS $SRCS $ZLIBS -o "$HERE/monomesh-vmap" -lpthread -lm
fi

echo "[+] Binario: $HERE/monomesh-vmap"
