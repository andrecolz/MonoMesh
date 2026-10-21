/* MonoMesh shared map core - common types and conventions.
 *
 * This library is compiled twice: once on the host (tools) and once on the
 * device (ESP32-S3, Arduino/ESP-IDF).  It must therefore stay dependency free:
 * no libpng, no Arduino headers, no POSIX-only calls, no recursion on hot
 * paths, and no floating point in the device path (the S3 emulates double).
 *
 * Fixed point convention
 * ----------------------
 * All rasteriser coordinates are 24.8 fixed point (int32_t, 1/256 px).  The
 * world -> viewport transform is applied to view-relative coordinates BEFORE
 * the multiplications so the intermediates stay inside int32:
 *
 *   vx = cx + ((int32_t)local_x * scale >> MM_FP_SHIFT)
 *
 * with local_x bounded by the chunk cell (0..~4096 + margin in 1/16 px units)
 * and scale bounded by the fractional zoom range (see mm_view).  The bounds
 * are part of the format contract and are verified by the property tests.
 */
#ifndef MM_CORE_H
#define MM_CORE_H

#include <stddef.h>
#include <stdint.h>

#define MM_FP_SHIFT 8
#define MM_FP_ONE (1 << MM_FP_SHIFT)

/* Container versions (see tools/native/vmap.c for the field by field spec). */
#define MM_VMAP_MAGIC "MONOMMAP"
#define MM_VMAP_VERSION 2
/* Version of the shared style tables (libmapcore/style.c).  A container whose
 * style version does not match the compiled style is refused by the reader. */
#define MM_STYLE_VERSION 1

/* way class ids carried in the container (must match style.c) */
enum {
    /* highway classes: same order/priority as ROAD_ORDER in the tools */
    MM_CLS_PEDESTRIAN = 0, MM_CLS_PATH = 1, MM_CLS_TRACK = 2, MM_CLS_SERVICE = 3,
    MM_CLS_LIVING_STREET = 4, MM_CLS_RESIDENTIAL = 5, MM_CLS_UNCLASSIFIED = 6,
    MM_CLS_TERTIARY = 7, MM_CLS_TERTIARY_LINK = 8, MM_CLS_SECONDARY = 9,
    MM_CLS_SECONDARY_LINK = 10, MM_CLS_PRIMARY = 11, MM_CLS_PRIMARY_LINK = 12,
    MM_CLS_TRUNK = 13, MM_CLS_TRUNK_LINK = 14, MM_CLS_MOTORWAY = 15,
    MM_CLS_MOTORWAY_LINK = 16,
    /* water classes */
    MM_CLS_WATER_STREAM = 17, MM_CLS_WATER_RIVER = 18, MM_CLS_WATER_AREA = 19,
    MM_CLS_WATER_RING_OUTER = 20, MM_CLS_WATER_RING_INNER = 21,
    /* rail */
    MM_CLS_RAILWAY = 22,
    MM_CLS_COUNT
};

/* per-record flags in the chunk payload (3 bits, packed with the class in one
 * byte: class = byte & 0x1f, flags = byte >> 5) */
enum {
    MM_REC_FILL = 1 << 0,       /* closed ring that must be filled (not just outlined) */
    MM_REC_BUTT_START = 1 << 1, /* artificial start: butt cap, no join/disk */
    MM_REC_BUTT_END = 1 << 2,   /* artificial end: butt cap, no join/disk */
};

/* A record's point count is stored as one hint byte (npts - 2), so records up
 * to 256 points are 2 bytes of header; the hint value 255 is the escape: an
 * unsigned LEB128 count follows. */
#define MM_REC_NPTS_MAX_INLINE 256
#define MM_REC_NPTS_ESCAPE 255

/* Fixed point point in view space (24.8). */
typedef struct {
    int32_t x, y;
} mm_pt;

/* Axis aligned rectangle in view space (24.8), inclusive min, exclusive max
 * semantics are not assumed: the rasterisers use closed intervals. */
typedef struct {
    int32_t x0, y0, x1, y1;
} mm_rect;

/* Drawing canvas: caller owned 8-bit greyscale buffer.  The rasteriser only
 * writes pixels inside the clip rect, so a "strip" repaint is just a canvas
 * with a narrower clip.  `scratch` is a caller owned workspace reused by the
 * polygon fills (the device loop has an 8 KB stack: no alloca, and a malloc
 * per stroke segment would dominate the frame time).  When the workspace is
 * too small the rasteriser falls back to malloc. */
typedef struct {
    uint8_t *pixels;   /* width * height bytes */
    int32_t width, height;
    int32_t stride;    /* bytes per row, >= width */
    mm_rect clip;      /* 24.8 pixel bounds; rows/cols outside are untouched */
    void *scratch;     /* optional workspace, may be NULL */
    size_t scratch_len;
} mm_canvas;

#endif /* MM_CORE_H */
