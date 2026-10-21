/* Shared helpers: style tables, path helpers, timing. */
#include "monomesh.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>

/* Must stay in sync with ROAD_ORDER in tools/mmap_converter.py */
static const char *const ROAD_ORDER[] = {
    "pedestrian", "path", "track", "service", "living_street",
    "residential", "unclassified", "tertiary", "tertiary_link",
    "secondary", "secondary_link", "primary", "primary_link",
    "trunk", "trunk_link", "motorway", "motorway_link",
};
#define ROAD_COUNT ((int)(sizeof(ROAD_ORDER) / sizeof(ROAD_ORDER[0])))

int mm_road_prio(const char *cls)
{
    for (int i = 0; i < ROAD_COUNT; i++) {
        if (strcmp(cls, ROAD_ORDER[i]) == 0)
            return i;
    }
    return -1;
}

const char *mm_road_name(int prio)
{
    if (prio < 0 || prio >= ROAD_COUNT)
        return "";
    return ROAD_ORDER[prio];
}

/* ---- zoom dependent style (must match tools/mmap_converter.py) ---------- */

typedef struct {
    int zmin;
    const char *cls;
} zoom_class;

static const zoom_class ZOOM_ROADS[] = {
    {0, "motorway"}, {0, "trunk"}, {0, "motorway_link"}, {0, "trunk_link"},
    {8, "primary"}, {8, "primary_link"},
    {10, "secondary"}, {10, "secondary_link"},
    {12, "tertiary"}, {12, "tertiary_link"}, {12, "unclassified"},
    {13, "residential"}, {13, "living_street"},
    {14, "service"}, {14, "track"}, {14, "pedestrian"},
    {15, "path"},
};

static int road_shown_at(const char *cls, int z)
{
    for (size_t i = 0; i < sizeof(ZOOM_ROADS) / sizeof(ZOOM_ROADS[0]); i++)
        if (strcmp(ZOOM_ROADS[i].cls, cls) == 0)
            return z >= ZOOM_ROADS[i].zmin;
    return 0;
}

double mm_water_min_size(int z)
{
    if (z <= 9)
        return 5.0;
    if (z <= 11)
        return 3.0;
    if (z <= 12)
        return 2.0;
    return 0.0;
}

int mm_water_kind_drawn(int kind, int z)
{
    switch (kind) {
    case MM_WATER_RING_OUTER:
    case MM_WATER_RING_INNER:
        return 1;
    case MM_WATER_AREA:
        return z >= 10;
    case MM_WATER_RIVER:
        return z >= 10;
    default:                      /* streams, ditches */
        return z >= 13;
    }
}

int mm_label_kind_drawn(int kind, int z)
{
    if (kind == MM_PLACE_CITY)
        return 1;
    if (kind == MM_PLACE_TOWN)
        return z >= 10;
    if (kind == MM_PLACE_VILLAGE || kind == MM_PLACE_SUBURB)
        return z >= 13;
    if (kind == MM_PLACE_HAMLET || kind == MM_PLACE_NEIGHBOURHOOD)
        return z >= 14;
    return 0;
}

int mm_label_budget(int z)
{
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

int mm_label_score(int kind, int32_t population)
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

int mm_road_width(int prio, int z, int *has_casing)
{
    const char *cls = mm_road_name(prio);
    int width = 1;
    int casing = 0;

    if (!road_shown_at(cls, z)) {
        if (has_casing)
            *has_casing = 0;
        return 0;
    }

    if (strcmp(cls, "residential") == 0 || strcmp(cls, "unclassified") == 0)
        width = (z >= 14) ? 2 : 1;
    else if (strcmp(cls, "tertiary") == 0 || strcmp(cls, "tertiary_link") == 0 ||
             strcmp(cls, "secondary_link") == 0)
        width = 2;
    else if (strcmp(cls, "secondary") == 0)
        width = (z >= 13) ? 3 : 2;
    else if (strcmp(cls, "primary") == 0)
        width = (z >= 13) ? 4 : 3;
    else if (strcmp(cls, "primary_link") == 0 || strcmp(cls, "trunk_link") == 0 ||
             strcmp(cls, "motorway_link") == 0)
        width = 3;
    else if (strcmp(cls, "trunk") == 0)
        width = (z >= 13) ? 5 : 4;
    else if (strcmp(cls, "motorway") == 0)
        width = (z >= 13) ? 6 : 4;

    if (strcmp(cls, "trunk") == 0 || strcmp(cls, "motorway") == 0)
        casing = 1;

    /* low zoom: hairlines, like the reference style */
    if (z <= 9) {
        if (strcmp(cls, "motorway") == 0 || strcmp(cls, "trunk") == 0 ||
            strcmp(cls, "motorway_link") == 0 || strcmp(cls, "trunk_link") == 0)
            width = 2;
        else
            width = 1;
    } else if (z <= 12) {
        if (width > 2)
            width = 2;
    }
    if (!(z >= 10 && width >= 3))
        casing = 0;

    if (has_casing)
        *has_casing = casing;
    return width;
}

int mm_water_line_width(int z)
{
    return (z >= 13) ? 3 : 2;
}

double mm_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* mkdir -p */
int mm_mkpath(const char *path)
{
    char tmp[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

void mm_data_free(mm_data *d)
{
    if (!d)
        return;
    free(d->ways);
    free(d->lat_e5);
    free(d->lon_e5);
    free(d->place_lat_e5);
    free(d->place_lon_e5);
    free(d->place_kind);
    free(d->place_population);
    free(d->name_off);
    free(d->name_len);
    free(d->names);
    memset(d, 0, sizeof(*d));
}
