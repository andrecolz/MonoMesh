#include "mm_inflate.h"

#include <string.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)

/* ESP32-S3 ROM miniz (tinfl). The state struct is kept on the heap by the
 * caller; we overlay it on ctx->state (a tinfl_decompressor is a few KB). */
#if defined(__has_include)
#  if __has_include(<esp32s3/rom/miniz.h>)
#    include <esp32s3/rom/miniz.h>
#    define MM_ROM_TINFL 1
#  elif __has_include(<rom/miniz.h>)
#    include <rom/miniz.h>
#    define MM_ROM_TINFL 1
#  endif
#endif
#ifndef MM_ROM_TINFL
#  include <miniz.h>
#endif

/* Guard: with a state struct larger than the caller reserved, inflate would
 * fail (or corrupt) at runtime - fail the build instead. */
_Static_assert(sizeof(tinfl_decompressor) <= MM_INFLATE_STATE_SIZE,
               "MM_INFLATE_STATE_SIZE too small for this ROM miniz");

int mm_inflate_init(mm_inflate_ctx *ctx)
{
    (void)ctx;
    return 0;
}

void mm_inflate_end(mm_inflate_ctx *ctx)
{
    (void)ctx;
}

int mm_inflate(mm_inflate_ctx *ctx, const void *src, size_t src_len,
               void *dst, size_t dst_cap, size_t *out_len)
{
    if (!ctx || !src || !dst)
        return -1;
    if (sizeof(tinfl_decompressor) > sizeof(ctx->state))
        return -1;
    tinfl_decompressor *dec = (tinfl_decompressor *)(void *)ctx->state;
    tinfl_init(dec);
    size_t in_len = src_len;
    size_t produced = dst_cap;
    const tinfl_status status = tinfl_decompress(
        dec, (const mz_uint8 *)src, &in_len, (mz_uint8 *)dst, (mz_uint8 *)dst,
        &produced, TINFL_FLAG_PARSE_ZLIB_HEADER |
                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE)
        return -1;
    if (out_len)
        *out_len = produced;
    return 0;
}

#else /* ------------------------------------------------ host: zlib ------- */

#include <zlib.h>

typedef struct {
    z_stream zs;
    int ready;
} mm_zlib_state;

int mm_inflate_init(mm_inflate_ctx *ctx)
{
    if (sizeof(mm_zlib_state) > sizeof(ctx->state))
        return -1;
    mm_zlib_state *st = (mm_zlib_state *)(void *)ctx->state;
    memset(st, 0, sizeof(*st));
    st->zs.opaque = Z_NULL;
    st->zs.zalloc = Z_NULL;
    st->zs.zfree = Z_NULL;
    if (inflateInit(&st->zs) != Z_OK)
        return -1;
    st->ready = 1;
    return 0;
}

void mm_inflate_end(mm_inflate_ctx *ctx)
{
    mm_zlib_state *st = (mm_zlib_state *)(void *)ctx->state;
    if (st->ready)
        inflateEnd(&st->zs);
    st->ready = 0;
}

int mm_inflate(mm_inflate_ctx *ctx, const void *src, size_t src_len,
               void *dst, size_t dst_cap, size_t *out_len)
{
    mm_zlib_state *st = (mm_zlib_state *)(void *)ctx->state;
    if (!st->ready)
        return -1;
    inflateReset(&st->zs);
    st->zs.next_in = (Bytef *)(uintptr_t)src;
    st->zs.avail_in = (uInt)src_len;
    st->zs.next_out = (Bytef *)dst;
    st->zs.avail_out = (uInt)dst_cap;
    int rc = inflate(&st->zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        /* malformed stream or output overflow: both are caller-visible errors */
        return -1;
    }
    if (out_len)
        *out_len = dst_cap - st->zs.avail_out;
    return 0;
}

#endif
