#include "mm_style.h"

/* ------------------------------------------------------------------ zoom -- */

static inline int32_t z_floor(int32_t z8)
{
    return z8 >> 8;
}

static inline int32_t z_frac(int32_t z8)
{
    return z8 & 0xff;
}

/* Linear interpolation between two integer zoom steps in 24.8 px, quantised
 * to 0.5 px steps as mandated by plan D4. */
static int32_t interp_width(int32_t w0, int32_t w1, int32_t frac8)
{
    int64_t w24 = ((int64_t)w0 << 8) + ((int64_t)(w1 - w0) * frac8);
    return (int32_t)(((w24 + 64) / 128) * 128);
}

/* --------------------------------------------------------------- roads ---- */

static const char *const CLASS_NAMES[MM_CLS_COUNT] = {
    "pedestrian", "path", "track", "service", "living_street",
    "residential", "unclassified", "tertiary", "tertiary_link",
    "secondary", "secondary_link", "primary", "primary_link",
    "trunk", "trunk_link", "motorway", "motorway_link",
    "water_stream", "water_river", "water_area",
    "water_ring_outer", "water_ring_inner", "railway",
};

const char *mm_style_class_name(int cls)
{
    if (cls < 0 || cls >= MM_CLS_COUNT)
        return "?";
    return CLASS_NAMES[cls];
}

static int road_class(int cls)
{
    return cls >= MM_CLS_PEDESTRIAN && cls <= MM_CLS_MOTORWAY_LINK;
}

/* First zoom step at which a road class appears (mirrors ZOOM_ROADS in the
 * raster pipeline). */
static int road_zmin(int cls)
{
    switch (cls) {
    case MM_CLS_MOTORWAY: case MM_CLS_MOTORWAY_LINK:
    case MM_CLS_TRUNK: case MM_CLS_TRUNK_LINK:
        return 0;
    case MM_CLS_PRIMARY: case MM_CLS_PRIMARY_LINK:
        return 8;
    case MM_CLS_SECONDARY: case MM_CLS_SECONDARY_LINK:
        return 10;
    case MM_CLS_TERTIARY: case MM_CLS_TERTIARY_LINK:
    case MM_CLS_UNCLASSIFIED:
        return 12;
    case MM_CLS_RESIDENTIAL: case MM_CLS_LIVING_STREET:
        return 13;
    case MM_CLS_SERVICE: case MM_CLS_TRACK: case MM_CLS_PEDESTRIAN:
        return 14;
    case MM_CLS_PATH:
        return 15;
    }
    return 99;
}

/* Width in px at an integer zoom step, before the half pixel quantisation. */
static int road_width_step(int cls, int z)
{
    if (z < road_zmin(cls))
        return 0;
    int w = 1;
    switch (cls) {
    case MM_CLS_RESIDENTIAL: case MM_CLS_UNCLASSIFIED:
        w = (z >= 14) ? 2 : 1;
        break;
    case MM_CLS_TERTIARY: case MM_CLS_TERTIARY_LINK:
    case MM_CLS_SECONDARY_LINK:
        w = 2;
        break;
    case MM_CLS_SECONDARY:
        w = (z >= 13) ? 3 : 2;
        break;
    case MM_CLS_PRIMARY:
        w = (z >= 13) ? 4 : 3;
        break;
    case MM_CLS_PRIMARY_LINK: case MM_CLS_TRUNK_LINK: case MM_CLS_MOTORWAY_LINK:
        w = 3;
        break;
    case MM_CLS_TRUNK:
        w = (z >= 13) ? 5 : 4;
        break;
    case MM_CLS_MOTORWAY:
        w = (z >= 13) ? 6 : 4;
        break;
    default:
        w = 1;
        break;
    }
    /* low zoom: hairlines; mid zoom: nothing thicker than 2 px */
    if (z <= 9) {
        if (cls == MM_CLS_MOTORWAY || cls == MM_CLS_TRUNK ||
            cls == MM_CLS_MOTORWAY_LINK || cls == MM_CLS_TRUNK_LINK)
            w = 2;
        else
            w = 1;
    } else if (z <= 12) {
        if (w > 2)
            w = 2;
    }
    return w;
}

static int road_casing_step(int cls, int z)
{
    int casing = (cls == MM_CLS_TRUNK || cls == MM_CLS_MOTORWAY);
    int w = road_width_step(cls, z);
    return casing && z >= 10 && w >= 3;
}

int32_t mm_style_road_width(int cls, int32_t z8, int *casing)
{
    if (z8 < 0)
        z8 = 0;
    if (!road_class(cls)) {
        if (casing)
            *casing = 0;
        return 0;
    }
    int32_t zf = z_floor(z8), frac = z_frac(z8);
    int32_t w0 = road_width_step(cls, (int)zf);
    int32_t w1 = road_width_step(cls, (int)(zf + 1));
    if (w0 == 0 && w1 == 0) {
        if (casing)
            *casing = 0;
        return 0;
    }
    if (casing)
        *casing = road_casing_step(cls, (int)(frac >= 128 ? zf + 1 : zf));
    return interp_width(w0, w1, frac);
}

int mm_style_class_visible(int cls, int32_t z8)
{
    if (road_class(cls))
        return z8 >= road_zmin(cls) * 256;
    switch (cls) {
    case MM_CLS_WATER_RING_OUTER: case MM_CLS_WATER_RING_INNER:
        return 1;
    case MM_CLS_WATER_AREA: case MM_CLS_WATER_RIVER:
        return z8 >= 10 * 256;
    case MM_CLS_WATER_STREAM:
        return z8 >= 13 * 256;
    case MM_CLS_RAILWAY:
        return 1;
    }
    return 0;
}

int32_t mm_style_water_line_width(int32_t z8)
{
    int32_t zf = z_floor(z8), frac = z_frac(z8);
    int32_t w0 = (zf >= 13) ? 3 : 2;
    int32_t w1 = (zf + 1 >= 13) ? 3 : 2;
    return interp_width(w0, w1, frac);
}

int32_t mm_style_water_min_size(int32_t z8)
{
    /* in px at the current zoom, only the integer steps matter here */
    int32_t z = z_floor(z8);
    if (z <= 9)
        return 5 * MM_FP_ONE;
    if (z <= 11)
        return 3 * MM_FP_ONE;
    if (z <= 12)
        return 2 * MM_FP_ONE;
    return 0;
}

int mm_style_is_water_fill(int cls)
{
    return cls == MM_CLS_WATER_AREA || cls == MM_CLS_WATER_RING_OUTER ||
           cls == MM_CLS_WATER_RING_INNER;
}

int mm_style_draw_order(int cls)
{
    switch (cls) {
    case MM_CLS_WATER_AREA: case MM_CLS_WATER_RING_OUTER:
        return 0;                       /* water fills */
    case MM_CLS_WATER_RING_INNER:
        return 1;                       /* island holes punched on the fills */
    case MM_CLS_WATER_RIVER: case MM_CLS_WATER_STREAM:
        return 2;                       /* water lines */
    case MM_CLS_RAILWAY:
        return 20;                      /* rails last (plan D2) */
    default:
        return 3 + cls;                 /* roads minor -> major */
    }
}

/* -------------------------------------------------------------- labels ---- */

int mm_style_label_visible(int kind, int32_t z8)
{
    switch (kind) {
    case MM_PLACE_CITY:
        return 1;
    case MM_PLACE_TOWN:
        return z8 >= 10 * 256;
    case MM_PLACE_VILLAGE: case MM_PLACE_SUBURB:
        return z8 >= 13 * 256;
    case MM_PLACE_HAMLET: case MM_PLACE_NEIGHBOURHOOD:
        return z8 >= 14 * 256;
    }
    return 0;
}

int mm_style_label_font(int kind, int32_t z8)
{
    switch (kind) {
    case MM_PLACE_CITY:
        return MM_FONT_CITY;
    case MM_PLACE_TOWN:
        return MM_FONT_TOWN;
    case MM_PLACE_VILLAGE: case MM_PLACE_SUBURB:
        return (z8 >= 13 * 256) ? MM_FONT_VILLAGE : -1;
    case MM_PLACE_HAMLET: case MM_PLACE_NEIGHBOURHOOD:
        return (z8 >= 14 * 256) ? MM_FONT_HAMLET : -1;
    }
    return -1;
}

int mm_style_label_dot_radius(int kind)
{
    switch (kind) {
    case MM_PLACE_CITY:
        return 4;
    case MM_PLACE_TOWN:
        return 3;
    case MM_PLACE_VILLAGE: case MM_PLACE_SUBURB:
    case MM_PLACE_HAMLET: case MM_PLACE_NEIGHBOURHOOD:
        return 2;
    }
    return 2;
}

int mm_style_label_budget(int32_t z8)
{
    int32_t z = z_floor(z8);
    if (z <= 7)
        return 3;
    if (z <= 9)
        return 6;
    if (z <= 11)
        return 9;
    if (z <= 13)
        return 14;
    return 20;
}

int32_t mm_style_label_score(int kind, int32_t population)
{
    if (population > 0)
        return population;
    switch (kind) {
    case MM_PLACE_CITY: return 100000;
    case MM_PLACE_TOWN: return 30000;
    case MM_PLACE_VILLAGE: return 5000;
    case MM_PLACE_SUBURB: return 3000;
    default: return 1000;
    }
}
