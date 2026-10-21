/* Binary cache reader/writer - byte compatible with the Python implementation. */
#include "monomesh.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int read_exact(FILE *f, void *buf, size_t n)
{
    return n == 0 || fread(buf, 1, n, f) == n;
}

static int write_exact(FILE *f, const void *buf, size_t n)
{
    return n == 0 || fwrite(buf, 1, n, f) == n;
}

int mm_cache_read(const char *path, mm_data *d)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    memset(d, 0, sizeof(*d));

    char magic[MM_CACHE_MAGIC_LEN];
    uint32_t version = 0, flags = 0, names_len = 0;
    uint32_t n_ways = 0, n_places = 0;
    if (!read_exact(f, magic, sizeof(magic)) || memcmp(magic, MM_CACHE_MAGIC, sizeof(magic)) != 0)
        goto fail;
    if (!read_exact(f, &version, 4) || !read_exact(f, &flags, 4))
        goto fail;
    if (version != 1 && version != MM_CACHE_VERSION)
        goto fail;
    if (!read_exact(f, d->bbox, sizeof(double) * 4))
        goto fail;
    if (!read_exact(f, &n_ways, 4) || !read_exact(f, &n_places, 4) || !read_exact(f, &names_len, 4))
        goto fail;

    d->n_ways = (int32_t)n_ways;
    d->n_places = (int32_t)n_places;
    d->names_len = names_len;

    uint8_t *types = malloc(n_ways ? n_ways : 1);
    uint8_t *prio = malloc(n_ways ? n_ways : 1);
    uint8_t *closed = malloc(n_ways ? n_ways : 1);
    int32_t *npts = malloc(sizeof(int32_t) * (n_ways ? n_ways : 1));
    if (!types || !prio || !closed || !npts) {
        free(types); free(prio); free(closed); free(npts);
        goto fail;
    }
    if (!read_exact(f, types, n_ways) || !read_exact(f, prio, n_ways) ||
        !read_exact(f, closed, n_ways) || !read_exact(f, npts, sizeof(int32_t) * n_ways)) {
        free(types); free(prio); free(closed); free(npts);
        goto fail;
    }
    int64_t n_points = 0;
    for (uint32_t i = 0; i < n_ways; i++)
        n_points += npts[i];
    d->n_points = (int32_t)n_points;
    d->lat_e5 = malloc(sizeof(int32_t) * (n_points ? n_points : 1));
    d->lon_e5 = malloc(sizeof(int32_t) * (n_points ? n_points : 1));
    d->ways = malloc(sizeof(mm_way) * (n_ways ? n_ways : 1));
    if (!d->lat_e5 || !d->lon_e5 || !d->ways) {
        free(types); free(prio); free(closed); free(npts);
        goto fail;
    }
    if (!read_exact(f, d->lat_e5, sizeof(int32_t) * n_points) ||
        !read_exact(f, d->lon_e5, sizeof(int32_t) * n_points)) {
        free(types); free(prio); free(closed); free(npts);
        goto fail;
    }
    int32_t off = 0;
    for (uint32_t i = 0; i < n_ways; i++) {
        d->ways[i].type = types[i];
        d->ways[i].prio = prio[i];
        d->ways[i].closed = closed[i];
        d->ways[i].npts = npts[i];
        d->ways[i].off = off;
        off += npts[i];
    }
    free(types); free(prio); free(closed); free(npts);

    size_t np = n_places ? n_places : 1;
    d->place_lat_e5 = malloc(sizeof(int32_t) * np);
    d->place_lon_e5 = malloc(sizeof(int32_t) * np);
    d->place_kind = malloc(np);
    d->place_population = malloc(sizeof(int32_t) * np);
    d->name_off = malloc(sizeof(int32_t) * np);
    d->name_len = malloc(sizeof(uint16_t) * np);
    d->names = malloc(names_len ? names_len : 1);
    if (!d->place_lat_e5 || !d->place_lon_e5 || !d->place_kind || !d->place_population ||
        !d->name_off ||
        !d->name_len || !d->names)
        goto fail;
    memset(d->place_population, 0, sizeof(int32_t) * np);
    if (!read_exact(f, d->place_lat_e5, sizeof(int32_t) * n_places) ||
        !read_exact(f, d->place_lon_e5, sizeof(int32_t) * n_places) ||
        !read_exact(f, d->place_kind, n_places))
        goto fail;
    if (version >= 2 &&
        !read_exact(f, d->place_population, sizeof(int32_t) * n_places))
        goto fail;
    if (!read_exact(f, d->name_off, sizeof(int32_t) * n_places) ||
        !read_exact(f, d->name_len, sizeof(uint16_t) * n_places) ||
        !read_exact(f, d->names, names_len))
        goto fail;

    fclose(f);
    return 0;

fail:
    fclose(f);
    mm_data_free(d);
    return -1;
}

int mm_cache_write(const char *path, const mm_data *d)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t version = MM_CACHE_VERSION, flags = 0;
    uint32_t n_ways = (uint32_t)d->n_ways, n_places = (uint32_t)d->n_places;
    uint32_t names_len = d->names_len;
    int ok = write_exact(f, MM_CACHE_MAGIC, MM_CACHE_MAGIC_LEN) &&
             write_exact(f, &version, 4) && write_exact(f, &flags, 4) &&
             write_exact(f, d->bbox, sizeof(double) * 4) &&
             write_exact(f, &n_ways, 4) && write_exact(f, &n_places, 4) &&
             write_exact(f, &names_len, 4);
    /* the format stores three parallel byte arrays (see the Python writer) */
    for (int32_t i = 0; ok && i < d->n_ways; i++)
        ok = write_exact(f, &d->ways[i].type, 1);
    for (int32_t i = 0; ok && i < d->n_ways; i++)
        ok = write_exact(f, &d->ways[i].prio, 1);
    for (int32_t i = 0; ok && i < d->n_ways; i++)
        ok = write_exact(f, &d->ways[i].closed, 1);
    for (int32_t i = 0; ok && i < d->n_ways; i++)
        ok = write_exact(f, &d->ways[i].npts, 4);
    if (ok)
        ok = write_exact(f, d->lat_e5, sizeof(int32_t) * d->n_points) &&
             write_exact(f, d->lon_e5, sizeof(int32_t) * d->n_points);
    if (ok)
        ok = write_exact(f, d->place_lat_e5, sizeof(int32_t) * d->n_places) &&
             write_exact(f, d->place_lon_e5, sizeof(int32_t) * d->n_places) &&
             write_exact(f, d->place_kind, d->n_places) &&
             write_exact(f, d->place_population, sizeof(int32_t) * d->n_places) &&
             write_exact(f, d->name_off, sizeof(int32_t) * d->n_places) &&
             write_exact(f, d->name_len, sizeof(uint16_t) * d->n_places) &&
             write_exact(f, d->names, d->names_len);
    fclose(f);
    return ok ? 0 : -1;
}
