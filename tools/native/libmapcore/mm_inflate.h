/* One-shot inflate behind a single interface (plan D6): zlib on the host,
 * the ESP32-S3 ROM tinfl on the device.  The decompressor state lives on the
 * heap (the Arduino loop task stack is 8 KB) and is passed in by the caller
 * so a chunk cache can own one state per concurrent reader.
 *
 * Chunk payloads are inflated in one shot: no incremental streaming, the
 * generator writes exactly one zlib stream per chunk (or none, when the
 * CHUNK_FLAG_RAW entry flag says the payload is stored uncompressed because
 * zlib would only make it bigger - measured on H band data).
 */
#ifndef MM_INFLATE_H
#define MM_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Bytes of state to allocate for one mm_inflate_ctx (rounded up generously). */
#if defined(ESP_PLATFORM) || defined(ARDUINO)
/* The ESP32-S3 ROM tinfl_decompressor is 10992 bytes on xtensa (the old
 * "~6 KB" estimate is wrong): keep the state big enough or every compressed
 * chunk decode fails at compile time instead of at runtime. */
#define MM_INFLATE_STATE_SIZE 12288
#else
#define MM_INFLATE_STATE_SIZE 512
#endif

typedef struct {
    unsigned char state[MM_INFLATE_STATE_SIZE];
} mm_inflate_ctx;

/* Allocates/frees the state. mm_inflate_init returns 0 on success. */
int mm_inflate_init(mm_inflate_ctx *ctx);
void mm_inflate_end(mm_inflate_ctx *ctx);

/* Inflates one zlib stream. Returns 0 on success and stores the produced
 * length in *out_len; -1 on malformed input or output overflow (nothing is
 * written past dst_cap).  The output buffer must be large enough for the
 * whole stream (chunks are decompressed in one piece, see above). */
int mm_inflate(mm_inflate_ctx *ctx, const void *src, size_t src_len,
               void *dst, size_t dst_cap, size_t *out_len);

#endif /* MM_INFLATE_H */
