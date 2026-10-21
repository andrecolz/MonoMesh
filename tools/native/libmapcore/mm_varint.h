/* Zigzag varint coding for chunk-local geometry deltas.
 *
 * Chosen over fixed int16 deltas in spike Fase 0 (scratch/spike_piano_b.md):
 * 0.50 B/coord vs 2.00 B/coord on realistic deltas, and the decode cost is a
 * rounding error next to the SD/inflate work (host: 934 MB/s).
 */
#ifndef MM_VARINT_H
#define MM_VARINT_H

#include <stddef.h>
#include <stdint.h>

/* Maximum bytes one varint can occupy (32 bit values). */
#define MM_VARINT_MAX 5

/* Writes v as unsigned LEB128. Returns bytes written (1..5), never 0. */
static inline int mm_varint_put(uint8_t *dst, uint32_t v)
{
    int n = 0;
    while (v >= 0x80u) {
        dst[n++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    dst[n++] = (uint8_t)v;
    return n;
}

/* Reads an unsigned LEB128. Stops after 5 bytes even on malformed input.
 * Returns bytes consumed (1..5), 0 when the buffer ends before the value.
 * *out is always written (partial values are returned, the caller validates). */
static inline int mm_varint_get(const uint8_t *src, size_t len, uint32_t *out)
{
    uint32_t v = 0;
    size_t n = 0;
    for (;;) {
        if (n >= len || n >= 5) {
            *out = v;
            return n ? (int)n : 0;
        }
        uint8_t b = src[n];
        v |= (uint32_t)(b & 0x7fu) << (7 * n);
        n++;
        if (!(b & 0x80u))
            break;
    }
    *out = v;
    return (int)n;
}

static inline uint32_t mm_zigzag_enc(int32_t v)
{
    return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

static inline int32_t mm_zigzag_dec(uint32_t v)
{
    return (int32_t)(v >> 1) ^ -(int32_t)(v & 1u);
}

#endif /* MM_VARINT_H */
