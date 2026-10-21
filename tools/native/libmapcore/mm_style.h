/* Shared style tables (plan D3): the ONE style of this turn, Toner-like.
 *
 * The container carries only class ids and a style_version; everything else -
 * widths, casings, min zooms, label policy - is derived from (class, zoom) by
 * the very same C source compiled into the host tools and into the firmware.
 * Nothing is looked up per element in the container: adding SEC_VSTYLE later
 * will not break the format.
 *
 * Zoom handling: the tables are keyed on the integer zoom step like the raster
 * pipeline always did.  For fractional zoom the exact width is scaled by
 * 2^frac and quantised to 0.5 px steps (plan D4: "spessori quantizzati a
 * 0.5px"), so a pinch zoom changes widths smoothly instead of popping.
 */
#ifndef MM_STYLE_H
#define MM_STYLE_H

#include "mm_core.h"

/* class id -> name (diagnostics/tests) */
const char *mm_style_class_name(int cls);

/* Road width in 24.8 px for a class at zoom z (may be fractional: 8.8 fixed
 * point, e.g. 13*256 = z13). Returns 0 when the class is not drawn at all.
 * *casing is set to 1 when the road needs the casing treatment (outline). */
int32_t mm_style_road_width(int cls, int32_t z8, int *casing);

/* Water line width in 24.8 px (0 for fillable rings: use mm_style_fill). */
int32_t mm_style_water_line_width(int32_t z8);

/* 1 when the class is visible at zoom z8. */
int mm_style_class_visible(int cls, int32_t z8);

/* Minimum projected size (px) for a water area at this zoom (tiny lakes are
 * dropped instead of flickering as 1px dots). */
int32_t mm_style_water_min_size(int32_t z8);

/* Fill colour for a water class in light style: 0 = the drawing colour, the
 * background is the caller's business (theme inversion lives in the view). */
int mm_style_is_water_fill(int cls);

/* Draw order band of a class (lower draws first): see chunk payload order. */
int mm_style_draw_order(int cls);

/* ---- labels ------------------------------------------------------------ */

enum { MM_PLACE_CITY = 0, MM_PLACE_TOWN = 1, MM_PLACE_VILLAGE = 2,
       MM_PLACE_SUBURB = 3, MM_PLACE_HAMLET = 4, MM_PLACE_NEIGHBOURHOOD = 5,
       MM_PLACE_OTHER = 255 };

enum { MM_FONT_CITY = 0, MM_FONT_TOWN = 1, MM_FONT_VILLAGE = 2, MM_FONT_HAMLET = 3 };

/* 1 when a place kind gets a label at this zoom. */
int mm_style_label_visible(int kind, int32_t z8);
/* Font variant for a place kind (or -1 when no label). */
int mm_style_label_font(int kind, int32_t z8);
/* Dot radius in px next to the label. */
int mm_style_label_dot_radius(int kind);
/* Per screen label budget. */
int mm_style_label_budget(int32_t z8);
/* Precomputed sort score (population wins, kind is the fallback). */
int32_t mm_style_label_score(int kind, int32_t population);

#endif /* MM_STYLE_H */
