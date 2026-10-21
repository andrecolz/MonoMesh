/* Minimal OSM PBF reader for the tile generator.
 *
 * Only what the renderer needs is decoded: roads/water/railways, place nodes
 * and the coordinates of the nodes referenced by those ways.  The file is read
 * into memory, blob boundaries are indexed once, then the zlib blobs are
 * inflated and parsed by a small thread pool.
 *
 * Two passes, like osmium's "complete ways" strategy:
 *   1. collect the node ids referenced by the interesting ways
 *   2. collect the coordinates of exactly those nodes
 */
#include "monomesh.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------ protobuf ---- */

static const unsigned char *pb_varint(const unsigned char *p, const unsigned char *end, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (p < end) {
        unsigned char b = *p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return p;
        }
        shift += 7;
        if (shift > 63)
            return NULL;
    }
    return NULL;
}

static const unsigned char *pb_field(const unsigned char *p, const unsigned char *end,
                                     uint32_t *field, uint32_t *wire)
{
    uint64_t key;
    p = pb_varint(p, end, &key);
    if (!p)
        return NULL;
    *field = (uint32_t)(key >> 3);
    *wire = (uint32_t)(key & 7);
    return p;
}

static inline int64_t zigzag(uint64_t v)
{
    return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
}

/* Skips a field of the given wire type (0/1/2/5). */
static const unsigned char *pb_skip(const unsigned char *p, const unsigned char *end, uint32_t wire)
{
    uint64_t v;
    switch (wire) {
    case 0:
        return pb_varint(p, end, &v);
    case 1:
        return (p + 8 <= end) ? p + 8 : NULL;
    case 2:
        p = pb_varint(p, end, &v);
        if (!p)
            return NULL;
        return (p + v <= end) ? p + v : NULL;
    case 5:
        return (p + 4 <= end) ? p + 4 : NULL;
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------- buffers ---- */

typedef struct {
    int64_t *v;
    size_t len, cap;
} i64buf;

static void i64buf_free(i64buf *b)
{
    free(b->v);
    b->v = NULL;
    b->len = b->cap = 0;
}

static int i64buf_push(i64buf *b, int64_t v)
{
    if (b->len == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 512;
        int64_t *nv = realloc(b->v, sizeof(int64_t) * ncap);
        if (!nv)
            return -1;
        b->v = nv;
        b->cap = ncap;
    }
    b->v[b->len++] = v;
    return 0;
}

static int decode_packed(const unsigned char *p, const unsigned char *end, i64buf *out,
                         int delta, int zig)
{
    int64_t acc = 0;
    out->len = 0;
    while (p < end) {
        uint64_t raw;
        p = pb_varint(p, end, &raw);
        if (!p)
            return -1;
        int64_t v = zig ? zigzag(raw) : (int64_t)raw;
        if (delta) {
            acc += v;
            v = acc;
        }
        if (i64buf_push(out, v) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ pass 1 data -- */

typedef struct {
    int32_t type, prio;
    int64_t ref_off;
    int32_t ref_cnt;
} way_rec;

typedef struct {
    way_rec *ways;
    size_t ways_len, ways_cap;
    int64_t *refs;
    size_t refs_len, refs_cap;
    /* ways that are members of a water multipolygon (usually untagged) */
    way_rec *mways;
    size_t mways_len, mways_cap;
    int64_t *mway_ids;              /* way id of every mways record (parallel array) */
    size_t mway_ids_cap;
    int64_t *mrefs;
    size_t mrefs_len, mrefs_cap;
    int32_t *pl_lat, *pl_lon, *pl_name_off;
    uint8_t *pl_kind;
    int32_t *pl_pop;
    uint16_t *pl_name_len;
    size_t pl_len, pl_cap;
    char *names;
    size_t names_len, names_cap;
    /* water relation members grouped per relation: rel_offsets[r]..rel_offsets[r+1]
       index into rel_ways/rel_roles (member way ids in relation order) */
    int64_t *rel_ways;
    size_t rel_ways_len, rel_ways_cap;
    uint8_t *rel_roles;
    size_t rel_roles_cap;
    int64_t *rel_offsets;
    size_t n_rel, rel_offsets_cap;
} pass1_out;

/* way id -> role in a water multipolygon (0 = outer, 1 = inner) */
typedef struct {
    int64_t *keys;
    uint8_t *roles;
    uint8_t *used;
    size_t count, cap;
} member_map;

struct task_ctx;
typedef struct task_ctx task_ctx;
struct node_table_s;
typedef struct node_table_s node_table;
static int reserve(void **ptr, size_t *cap, size_t need, size_t elem);

struct task_ctx {
    int pass;
    const unsigned char *file;
    const size_t *blob_off;
    const uint32_t *blob_len;
    size_t blob_count;
    size_t blob_from, blob_to;
    double bbox[4];
    pass1_out out;
    node_table *table;
    member_map members;   /* pass 1 only */
};


static void pass1_init(pass1_out *o)
{
    memset(o, 0, sizeof(*o));
}

static void pass1_free(pass1_out *o)
{
    free(o->ways);
    free(o->refs);
    free(o->mways);
    free(o->mway_ids);
    free(o->mrefs);
    free(o->pl_lat);
    free(o->pl_lon);
    free(o->pl_pop);
    free(o->pl_kind);
    free(o->pl_name_off);
    free(o->pl_name_len);
    free(o->names);
    free(o->rel_ways);
    free(o->rel_roles);
    free(o->rel_offsets);
    memset(o, 0, sizeof(*o));
}

static int pass1_add_member(pass1_out *o, int64_t way_id, const int64_t *refs, size_t nrefs)
{
    if (reserve((void **)&o->mways, &o->mways_cap, o->mways_len + 1, sizeof(way_rec)) != 0)
        return -1;
    if (reserve((void **)&o->mway_ids, &o->mway_ids_cap, o->mways_len + 1, sizeof(int64_t)) != 0)
        return -1;
    if (reserve((void **)&o->mrefs, &o->mrefs_cap, o->mrefs_len + nrefs, sizeof(int64_t)) != 0)
        return -1;
    way_rec *w = &o->mways[o->mways_len++];
    w->type = MM_WATER;
    w->prio = 0;                  /* the role is a property of the relation, not of the way */
    w->ref_off = (int64_t)o->mrefs_len;
    w->ref_cnt = (int32_t)nrefs;
    o->mway_ids[o->mways_len - 1] = way_id;
    memcpy(o->mrefs + o->mrefs_len, refs, sizeof(int64_t) * nrefs);
    o->mrefs_len += nrefs;
    return 0;
}

static int reserve(void **ptr, size_t *cap, size_t need, size_t elem)
{
    if (*cap >= need)
        return 0;
    size_t ncap = *cap ? *cap : 128;
    while (ncap < need)
        ncap *= 2;
    void *np = realloc(*ptr, ncap * elem);
    if (!np)
        return -1;
    *ptr = np;
    *cap = ncap;
    return 0;
}

static int pass1_add_way(pass1_out *o, int type, int prio, const int64_t *refs, size_t nrefs)
{
    if (reserve((void **)&o->ways, &o->ways_cap, o->ways_len + 1, sizeof(way_rec)) != 0)
        return -1;
    if (reserve((void **)&o->refs, &o->refs_cap, o->refs_len + nrefs, sizeof(int64_t)) != 0)
        return -1;
    way_rec *w = &o->ways[o->ways_len++];
    w->type = type;
    w->prio = prio;
    w->ref_off = (int64_t)o->refs_len;
    w->ref_cnt = (int32_t)nrefs;
    memcpy(o->refs + o->refs_len, refs, sizeof(int64_t) * nrefs);
    o->refs_len += nrefs;
    return 0;
}

static int pass1_ensure_places(pass1_out *o, size_t need)
{
    if (o->pl_cap >= need)
        return 0;
    size_t ncap = o->pl_cap ? o->pl_cap : 128;
    while (ncap < need)
        ncap *= 2;
    int32_t *lat = realloc(o->pl_lat, sizeof(int32_t) * ncap);
    int32_t *lon = realloc(o->pl_lon, sizeof(int32_t) * ncap);
    int32_t *pop = realloc(o->pl_pop, sizeof(int32_t) * ncap);
    int32_t *noff = realloc(o->pl_name_off, sizeof(int32_t) * ncap);
    uint8_t *kind = realloc(o->pl_kind, ncap);
    uint16_t *nlen = realloc(o->pl_name_len, sizeof(uint16_t) * ncap);
    if (!lat || !lon || !pop || !noff || !kind || !nlen) {
        free(lat);
        free(lon);
        free(pop);
        free(noff);
        free(kind);
        free(nlen);
        return -1;
    }
    o->pl_lat = lat;
    o->pl_lon = lon;
    o->pl_pop = pop;
    o->pl_name_off = noff;
    o->pl_kind = kind;
    o->pl_name_len = nlen;
    o->pl_cap = ncap;
    return 0;
}

static int pass1_add_place(pass1_out *o, int32_t lat_e5, int32_t lon_e5, int kind,
                           const char *name, size_t name_len, int32_t population)
{
    if (pass1_ensure_places(o, o->pl_len + 1) != 0)
        return -1;
    int32_t noff = 0;
    if (name && name_len) {
        if (reserve((void **)&o->names, &o->names_cap, o->names_len + name_len + 1, 1) != 0)
            return -1;
        noff = (int32_t)o->names_len;
        memcpy(o->names + o->names_len, name, name_len);
        o->names[o->names_len + name_len] = '\0';
        o->names_len += name_len + 1;
    }
    size_t i = o->pl_len++;
    o->pl_lat[i] = lat_e5;
    o->pl_lon[i] = lon_e5;
    o->pl_pop[i] = population;
    o->pl_kind[i] = (uint8_t)kind;
    o->pl_name_off[i] = noff;
    o->pl_name_len[i] = (uint16_t)(name_len > 65535 ? 65535 : name_len);
    return 0;
}

/* ------------------------------------------------------------- block ctx -- */

enum {
    K_HIGHWAY = 0, K_RAILWAY, K_WATERWAY, K_NATURAL, K_PLACE, K_NAME,
    K_TYPE, K_WATER_KEY, K_LANDUSE, K_POPULATION, K_COUNT
};

static const char *const KEY_NAMES[K_COUNT] = {
    "highway", "railway", "waterway", "natural", "place", "name",
    "type", "water", "landuse", "population"
};

typedef struct {
    int key_idx[K_COUNT];
    int32_t n_strings;
    const unsigned char **strings;
    uint32_t *str_lens;
} block_ctx;

static void block_ctx_free(block_ctx *b)
{
    free((void *)b->strings);
    free(b->str_lens);
    memset(b, 0, sizeof(*b));
}

static int parse_string_table(const unsigned char *p, const unsigned char *end, block_ctx *b)
{
    int32_t cap = 64;
    b->strings = malloc(sizeof(void *) * (size_t)cap);
    b->str_lens = malloc(sizeof(uint32_t) * (size_t)cap);
    b->n_strings = 0;
    if (!b->strings || !b->str_lens)
        return -1;
    while (p < end) {
        uint32_t f, w;
        p = pb_field(p, end, &f, &w);
        if (!p)
            return -1;
        if (f == 1 && w == 2) {
            uint64_t len;
            p = pb_varint(p, end, &len);
            if (!p || p + len > end)
                return -1;
            if (b->n_strings == cap) {
                cap *= 2;
                const unsigned char **ns = realloc((void *)b->strings, sizeof(void *) * (size_t)cap);
                uint32_t *nl = realloc(b->str_lens, sizeof(uint32_t) * (size_t)cap);
                if (!ns || !nl)
                    return -1;
                b->strings = ns;
                b->str_lens = nl;
            }
            b->strings[b->n_strings] = p;
            b->str_lens[b->n_strings] = (uint32_t)len;
            b->n_strings++;
            p += len;
        } else {
            p = pb_skip(p, end, w);
            if (!p)
                return -1;
        }
    }
    for (int k = 0; k < K_COUNT; k++) {
        b->key_idx[k] = -1;
        size_t klen = strlen(KEY_NAMES[k]);
        for (int32_t i = 0; i < b->n_strings; i++) {
            if (b->str_lens[i] == klen && memcmp(b->strings[i], KEY_NAMES[k], klen) == 0) {
                b->key_idx[k] = i;
                break;
            }
        }
    }
    return 0;
}

static int place_kind_of(const block_ctx *b, int32_t idx)
{
    static const char *const KINDS[] = {"city", "town", "village", "suburb", "hamlet", "neighbourhood"};
    if (idx < 0 || idx >= b->n_strings)
        return (int)MM_PLACE_OTHER;
    for (int i = 0; i < 6; i++) {
        size_t klen = strlen(KINDS[i]);
        if (b->str_lens[idx] == klen && memcmp(b->strings[idx], KINDS[i], klen) == 0)
            return i;
    }
    return (int)MM_PLACE_OTHER;
}

/* Parses the OSM population tag ('12345', '12 345', ...). */
static int32_t parse_population(const unsigned char *s, uint32_t len)
{
    int64_t v = 0;
    int digits = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (s[i] - '0');
            if (++digits > 9)
                return 0;
        }
    }
    return digits ? (int32_t)v : 0;
}

/* --- relation members (water multipolygons) ------------------------------- */

static int member_map_add(member_map *m, int64_t id, int role)
{
    if (m->count == m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 1024;
        int64_t *nk = malloc(sizeof(int64_t) * ncap);
        uint8_t *nr = malloc(ncap);
        uint8_t *nu = calloc(ncap, 1);
        if (!nk || !nr || !nu) {
            free(nk);
            free(nr);
            free(nu);
            return -1;
        }
        if (m->count) {
            memcpy(nk, m->keys, sizeof(int64_t) * m->count);
            memcpy(nr, m->roles, m->count);
            memcpy(nu, m->used, m->count);
        }
        free(m->keys);
        free(m->roles);
        free(m->used);
        m->keys = nk;
        m->roles = nr;
        m->used = nu;
        m->cap = ncap;
    }
    m->keys[m->count] = id;
    m->roles[m->count] = (uint8_t)role;
    m->used[m->count] = 1;
    m->count++;
    return 0;
}

static int is_water_relation_tags(const block_ctx *b, const i64buf *keys, const i64buf *vals)
{
    int32_t type = -1, nat = -1, wtr = -1, wwy = -1, landuse = -1;
    for (size_t i = 0; i < keys->len && i < vals->len; i++) {
        int32_t k = (int32_t)keys->v[i], v = (int32_t)vals->v[i];
        if (k == b->key_idx[K_TYPE]) type = v;
        else if (k == b->key_idx[K_NATURAL]) nat = v;
        else if (k == b->key_idx[K_WATER_KEY]) wtr = v;
        else if (k == b->key_idx[K_WATERWAY]) wwy = v;
        else if (k == b->key_idx[K_LANDUSE]) landuse = v;
    }
    if (type < 0 || type >= b->n_strings)
        return 0;
    if (b->str_lens[type] != 12 || memcmp(b->strings[type], "multipolygon", 12) != 0)
        return 0;
    if (nat >= 0 && nat < b->n_strings) {
        const unsigned char *v = b->strings[nat];
        uint32_t l = b->str_lens[nat];
        if ((l == 5 && memcmp(v, "water", 5) == 0) || (l == 7 && memcmp(v, "wetland", 7) == 0) ||
            (l == 3 && memcmp(v, "bay", 3) == 0))
            return 1;
    }
    if (wtr >= 0)
        return 1;
    if (wwy >= 0 && wwy < b->n_strings) {
        const unsigned char *v = b->strings[wwy];
        uint32_t l = b->str_lens[wwy];
        if ((l == 9 && memcmp(v, "riverbank", 9) == 0) || (l == 5 && memcmp(v, "river", 5) == 0) ||
            (l == 5 && memcmp(v, "canal", 5) == 0))
            return 1;
    }
    if (landuse >= 0 && landuse < b->n_strings) {
        const unsigned char *v = b->strings[landuse];
        uint32_t l = b->str_lens[landuse];
        if ((l == 9 && memcmp(v, "reservoir", 9) == 0) || (l == 5 && memcmp(v, "basin", 5) == 0))
            return 1;
    }
    return 0;
}

/* Relations (PrimitiveGroup field 4): collects water multipolygon members. */
static int handle_relation(const unsigned char *p, const unsigned char *end, const block_ctx *b,
                           task_ctx *t)
{
    i64buf keys = {0}, vals = {0}, roles = {0}, memids = {0}, types = {0};
    int rc = 0;
    while (p < end) {
        uint32_t f, w;
        p = pb_field(p, end, &f, &w);
        if (!p) { rc = -1; break; }
        if (w == 2) {
            uint64_t len;
            p = pb_varint(p, end, &len);
            if (!p || p + len > end) { rc = -1; break; }
            if (f == 2) rc = decode_packed(p, p + len, &keys, 0, 0);
            else if (f == 3) rc = decode_packed(p, p + len, &vals, 0, 0);
            else if (f == 8) rc = decode_packed(p, p + len, &roles, 0, 0);
            else if (f == 9) rc = decode_packed(p, p + len, &memids, 1, 1);
            else if (f == 10) rc = decode_packed(p, p + len, &types, 0, 0);
            if (rc != 0)
                break;
            p += len;
        } else {
            p = pb_skip(p, end, w);
            if (!p) { rc = -1; break; }
        }
    }
    if (rc == 0 && is_water_relation_tags(b, &keys, &vals)) {
        /* The members of this relation are stored as their own group: the ring
         * assembly must never chain them with the members of another relation
         * (that is how a lake ended up joined to the river leaving it). */
        int64_t group_start = (int64_t)t->out.rel_ways_len;
        for (size_t i = 0; i < memids.len && i < types.len; i++) {
            if (types.v[i] != 1)        /* only way members */
                continue;
            int role = 0;
            if (i < roles.len && roles.v[i] >= 0 && roles.v[i] < b->n_strings) {
                int32_t sid = (int32_t)roles.v[i];
                if (b->str_lens[sid] == 5 && memcmp(b->strings[sid], "inner", 5) == 0)
                    role = 1;
            }
            if (member_map_add(&t->members, memids.v[i], role) != 0) {
                rc = -1;
                break;
            }
            if (reserve((void **)&t->out.rel_ways, &t->out.rel_ways_cap,
                        t->out.rel_ways_len + 1, sizeof(int64_t)) != 0 ||
                reserve((void **)&t->out.rel_roles, &t->out.rel_roles_cap,
                        t->out.rel_ways_len + 1, 1) != 0) {
                rc = -1;
                break;
            }
            t->out.rel_ways[t->out.rel_ways_len] = memids.v[i];
            t->out.rel_roles[t->out.rel_ways_len] = (uint8_t)role;
            t->out.rel_ways_len++;
        }
        if (rc == 0) {
            if (reserve((void **)&t->out.rel_offsets, &t->out.rel_offsets_cap,
                        t->out.n_rel + 2, sizeof(int64_t)) != 0) {
                rc = -1;
            } else {
                if (t->out.n_rel == 0)
                    t->out.rel_offsets[0] = 0;
                t->out.n_rel++;
                t->out.rel_offsets[t->out.n_rel] = (int64_t)t->out.rel_ways_len;
                (void)group_start;
            }
        }
    }
    i64buf_free(&keys);
    i64buf_free(&vals);
    i64buf_free(&roles);
    i64buf_free(&memids);
    i64buf_free(&types);
    return rc;
}

/* ------------------------------------------------------------- node table -- */

typedef struct node_table_s {
    int64_t *keys;
    uint8_t *used;
    int32_t *lat_e5;
    int64_t *lon_e5;
    size_t cap;
} node_table;

static inline uint64_t hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static void table_insert(node_table *t, int64_t id)
{
    size_t mask = t->cap - 1;
    size_t i = (size_t)hash64((uint64_t)id) & mask;
    while (t->used[i]) {
        if (t->keys[i] == id)
            return;
        i = (i + 1) & mask;
    }
    t->used[i] = 1;
    t->keys[i] = id;
}

static inline int table_find(const node_table *t, int64_t id, size_t *slot)
{
    size_t mask = t->cap - 1;
    size_t i = (size_t)hash64((uint64_t)id) & mask;
    while (t->used[i]) {
        if (t->keys[i] == id) {
            *slot = i;
            return 1;
        }
        i = (i + 1) & mask;
    }
    return 0;
}

/* ----------------------------------------------------------- block parse -- */


static int handle_way(const unsigned char *p, const unsigned char *end, const block_ctx *b,
                      task_ctx *t, const member_map *members)
{
    i64buf keys = {0}, vals = {0}, refs = {0};
    int64_t id = 0;
    int rc = 0;
    while (p < end) {
        uint32_t f, w;
        p = pb_field(p, end, &f, &w);
        if (!p) { rc = -1; break; }
        if (w == 2) {
            uint64_t len;
            p = pb_varint(p, end, &len);
            if (!p || p + len > end) { rc = -1; break; }
            if (f == 2) rc = decode_packed(p, p + len, &keys, 0, 0);
            else if (f == 3) rc = decode_packed(p, p + len, &vals, 0, 0);
            else if (f == 8) rc = decode_packed(p, p + len, &refs, 1, 1);
            if (rc != 0)
                break;
            p += len;
        } else {
            if (w == 0 && f == 1) {
                uint64_t v;
                p = pb_varint(p, end, &v);
                if (!p) { rc = -1; break; }
                id = (int64_t)v;
            } else {
                p = pb_skip(p, end, w);
                if (!p) { rc = -1; break; }
            }
        }
    }

    if (rc == 0 && refs.len >= 2) {
        /* member of a water multipolygon? then it is part of a lake outline */
        if (members && members->count) {
            for (size_t k = 0; k < members->count; k++) {
                if (members->used[k] && members->keys[k] == id) {
                    if (pass1_add_member(&t->out, id, refs.v, refs.len) != 0)
                        rc = -1;
                    goto done;
                }
            }
        }
        int32_t hw = -1, rw = -1, ww = -1, nat = -1;
        for (size_t i = 0; i < keys.len && i < vals.len; i++) {
            int32_t k = (int32_t)keys.v[i], v = (int32_t)vals.v[i];
            if (k == b->key_idx[K_HIGHWAY]) hw = v;
            else if (k == b->key_idx[K_RAILWAY]) rw = v;
            else if (k == b->key_idx[K_WATERWAY]) ww = v;
            else if (k == b->key_idx[K_NATURAL]) nat = v;
        }
        int type = -1, prio = MM_PRIO_NONE;
        if (hw >= 0 && hw < b->n_strings) {
            char buf[64];
            size_t len = b->str_lens[hw] < sizeof(buf) - 1 ? b->str_lens[hw] : sizeof(buf) - 1;
            memcpy(buf, b->strings[hw], len);
            buf[len] = '\0';
            int pr = mm_road_prio(buf);
            if (pr >= 0) {
                type = MM_HIGHWAY;
                prio = pr;
            }
        }
        int water_kind = -1;
        if (nat >= 0 && nat < b->n_strings && b->str_lens[nat] == 5 &&
            memcmp(b->strings[nat], "water", 5) == 0)
            water_kind = MM_WATER_AREA;
        if (water_kind < 0 && ww >= 0 && ww < b->n_strings) {
            const unsigned char *v = b->strings[ww];
            uint32_t vl = b->str_lens[ww];
            if ((vl == 5 && memcmp(v, "river", 5) == 0) ||
                (vl == 5 && memcmp(v, "canal", 5) == 0) ||
                (vl == 7 && memcmp(v, "fairway", 7) == 0))
                water_kind = MM_WATER_RIVER;
            else if (vl == 9 && memcmp(v, "riverbank", 9) == 0)
                water_kind = MM_WATER_AREA;
            else
                water_kind = MM_WATER_STREAM;
        }
        if (type < 0 && water_kind >= 0) {
            type = MM_WATER;
            prio = water_kind;
        }
        if (type < 0 && rw >= 0)
            type = MM_RAILWAY;

        if (type >= 0 && pass1_add_way(&t->out, type, prio, refs.v, refs.len) != 0)
            rc = -1;
    }
done:
    i64buf_free(&keys);
    i64buf_free(&vals);
    i64buf_free(&refs);
    return rc;
}

static int handle_dense_nodes(const unsigned char *p, const unsigned char *end,
                              const block_ctx *b, task_ctx *t, int64_t lat_off, int64_t lon_off,
                              int64_t granularity)
{
    i64buf ids = {0}, lats = {0}, lons = {0}, kvs = {0};
    int rc = 0;
    while (p < end) {
        uint32_t f, w;
        p = pb_field(p, end, &f, &w);
        if (!p) { rc = -1; break; }
        if (w == 2) {
            uint64_t len;
            p = pb_varint(p, end, &len);
            if (!p || p + len > end) { rc = -1; break; }
            if (f == 1) rc = decode_packed(p, p + len, &ids, 1, 1);
            else if (f == 8) rc = decode_packed(p, p + len, &lats, 1, 1);
            else if (f == 9) rc = decode_packed(p, p + len, &lons, 1, 1);
            else if (f == 10) rc = decode_packed(p, p + len, &kvs, 0, 0);
            if (rc != 0)
                break;
            p += len;
        } else {
            p = pb_skip(p, end, w);
            if (!p) { rc = -1; break; }
        }
    }

    if (rc == 0) {
        size_t kv = 0;
        for (size_t i = 0; i < ids.len && i < lats.len && i < lons.len; i++) {
            double lat = 1e-9 * (double)(lat_off + granularity * lats.v[i]);
            double lon = 1e-9 * (double)(lon_off + granularity * lons.v[i]);
            int32_t lat_e5 = (int32_t)llround(lat * MM_E5);
            int32_t lon_e5 = (int32_t)llround(lon * MM_E5);

            int32_t place_idx = -1, name_idx = -1, pop_idx = -1;
            if (kv < kvs.len) {
                while (kv < kvs.len && kvs.v[kv] != 0) {
                    int32_t k = (int32_t)kvs.v[kv];
                    int32_t v = (kv + 1 < kvs.len) ? (int32_t)kvs.v[kv + 1] : -1;
                    if (k == b->key_idx[K_PLACE]) place_idx = v;
                    else if (k == b->key_idx[K_NAME]) name_idx = v;
                    else if (k == b->key_idx[K_POPULATION]) pop_idx = v;
                    kv += 2;
                }
                if (kv < kvs.len)
                    kv++; /* terminator */
            }

            if (t->pass == 2) {
                if (place_idx >= 0 && lat >= t->bbox[0] && lat <= t->bbox[2] &&
                    lon >= t->bbox[1] && lon <= t->bbox[3]) {
                    int kind = place_kind_of(b, place_idx);
                    if (kind != (int)MM_PLACE_OTHER) {
                        const char *nm = NULL;
                        size_t nl = 0;
                        int32_t pop = 0;
                        if (name_idx >= 0 && name_idx < b->n_strings) {
                            nm = (const char *)b->strings[name_idx];
                            nl = b->str_lens[name_idx];
                        }
                        if (pop_idx >= 0 && pop_idx < b->n_strings)
                            pop = parse_population(b->strings[pop_idx], b->str_lens[pop_idx]);
                        if (pass1_add_place(&t->out, lat_e5, lon_e5, kind, nm, nl, pop) != 0)
                            rc = -1;
                    }
                }
            } else {
                size_t slot;
                if (table_find(t->table, ids.v[i], &slot)) {
                    t->table->lat_e5[slot] = lat_e5;
                    t->table->lon_e5[slot] = lon_e5;
                }
            }
            if (rc != 0)
                break;
        }
    }
    i64buf_free(&ids);
    i64buf_free(&lats);
    i64buf_free(&lons);
    i64buf_free(&kvs);
    return rc;
}

static int handle_node(const unsigned char *p, const unsigned char *end, const block_ctx *b,
                       task_ctx *t, int64_t lat_off, int64_t lon_off, int64_t granularity)
{
    i64buf keys = {0}, vals = {0};
    int64_t id = 0, raw_lat = 0, raw_lon = 0;
    int rc = 0;
    while (p < end) {
        uint32_t f, w;
        p = pb_field(p, end, &f, &w);
        if (!p) { rc = -1; break; }
        if (w == 2) {
            uint64_t len;
            p = pb_varint(p, end, &len);
            if (!p || p + len > end) { rc = -1; break; }
            if (f == 2) rc = decode_packed(p, p + len, &keys, 0, 0);
            else if (f == 3) rc = decode_packed(p, p + len, &vals, 0, 0);
            if (rc != 0)
                break;
            p += len;
        } else if (w == 0) {
            uint64_t v;
            p = pb_varint(p, end, &v);
            if (!p) { rc = -1; break; }
            if (f == 1) id = zigzag(v);
            else if (f == 8) raw_lat = zigzag(v);
            else if (f == 9) raw_lon = zigzag(v);
        } else {
            p = pb_skip(p, end, w);
            if (!p) { rc = -1; break; }
        }
    }
    if (rc == 0) {
        double lat = 1e-9 * (double)(lat_off + granularity * raw_lat);
        double lon = 1e-9 * (double)(lon_off + granularity * raw_lon);
        if (t->pass == 2) {
            int32_t place_idx = -1, name_idx = -1, pop_idx = -1;
            for (size_t i = 0; i < keys.len && i < vals.len; i++) {
                if (keys.v[i] == b->key_idx[K_PLACE]) place_idx = (int32_t)vals.v[i];
                else if (keys.v[i] == b->key_idx[K_NAME]) name_idx = (int32_t)vals.v[i];
                else if (keys.v[i] == b->key_idx[K_POPULATION]) pop_idx = (int32_t)vals.v[i];
            }
            int kind = place_idx >= 0 ? place_kind_of(b, place_idx) : (int)MM_PLACE_OTHER;
            if (kind != (int)MM_PLACE_OTHER && lat >= t->bbox[0] && lat <= t->bbox[2] &&
                lon >= t->bbox[1] && lon <= t->bbox[3]) {
                const char *nm = NULL;
                size_t nl = 0;
                int32_t pop = 0;
                if (name_idx >= 0 && name_idx < b->n_strings) {
                    nm = (const char *)b->strings[name_idx];
                    nl = b->str_lens[name_idx];
                }
                if (pop_idx >= 0 && pop_idx < b->n_strings)
                    pop = parse_population(b->strings[pop_idx], b->str_lens[pop_idx]);
                rc = pass1_add_place(&t->out, (int32_t)llround(lat * MM_E5),
                                     (int32_t)llround(lon * MM_E5), kind, nm, nl, pop);
            }
        } else {
            size_t slot;
            if (table_find(t->table, id, &slot)) {
                t->table->lat_e5[slot] = (int32_t)llround(lat * MM_E5);
                t->table->lon_e5[slot] = llround(lon * MM_E5);
            }
        }
    }
    i64buf_free(&keys);
    i64buf_free(&vals);
    return rc;
}

static int process_primitive_block(const unsigned char *buf, size_t len, task_ctx *t)
{
    const unsigned char *end = buf + len;
    block_ctx b;
    memset(&b, 0, sizeof(b));
    int rc = 0;
    int64_t granularity = 100, lat_off = 0, lon_off = 0;

    /* header fields first: string table + granularity/offsets */
    const unsigned char *q = buf;
    while (q < end) {
        uint32_t f, w;
        q = pb_field(q, end, &f, &w);
        if (!q) { rc = -1; break; }
        if (f == 1 && w == 2) {
            uint64_t slen;
            q = pb_varint(q, end, &slen);
            if (!q || q + slen > end) { rc = -1; break; }
            if (parse_string_table(q, q + slen, &b) != 0)
                rc = -1;
            q += slen;
        } else if (f == 17 && w == 0) {
            uint64_t v;
            q = pb_varint(q, end, &v);
            if (!q) { rc = -1; break; }
            granularity = (int64_t)v;
        } else if (f == 19 && w == 0) {
            uint64_t v;
            q = pb_varint(q, end, &v);
            if (!q) { rc = -1; break; }
            lat_off = (int64_t)v;
        } else if (f == 20 && w == 0) {
            uint64_t v;
            q = pb_varint(q, end, &v);
            if (!q) { rc = -1; break; }
            lon_off = (int64_t)v;
        } else {
            q = pb_skip(q, end, w);
            if (!q) { rc = -1; break; }
        }
        if (rc != 0)
            break;
    }

    /* primitive groups */
    const unsigned char *p = buf;
    while (rc == 0 && p < end) {
        uint32_t f, w;
        p = pb_field(p, end, &f, &w);
        if (!p) { rc = -1; break; }
        if (f == 2 && w == 2) {
            uint64_t glen;
            p = pb_varint(p, end, &glen);
            if (!p || p + glen > end) { rc = -1; break; }
            const unsigned char *g = p, *gend = p + glen;
            while (rc == 0 && g < gend) {
                uint32_t gf, gw;
                g = pb_field(g, gend, &gf, &gw);
                if (!g) { rc = -1; break; }
                if (gw == 2) {
                    uint64_t slen;
                    g = pb_varint(g, gend, &slen);
                    if (!g || g + slen > gend) { rc = -1; break; }
                    if (gf == 1) {
                        if (t->pass == 2 || t->pass == 3)
                            rc = handle_node(g, g + slen, &b, t, lat_off, lon_off, granularity);
                    } else if (gf == 2) {
                        if (t->pass == 2 || t->pass == 3)
                            rc = handle_dense_nodes(g, g + slen, &b, t, lat_off, lon_off, granularity);
                    } else if (gf == 3) {
                        if (t->pass == 2)
                            rc = handle_way(g, g + slen, &b, t, t->members.count ? &t->members : NULL);
                    } else if (gf == 4) {
                        if (t->pass == 1)
                            rc = handle_relation(g, g + slen, &b, t);
                    }
                    g += slen;
                } else {
                    g = pb_skip(g, gend, gw);
                    if (!g) { rc = -1; break; }
                }
            }
            p += glen;
        } else {
            p = pb_skip(p, end, w);
            if (!p) { rc = -1; break; }
        }
    }
    block_ctx_free(&b);
    return rc;
}

/* ------------------------------------------------------------ blob worker -- */

static void *worker(void *arg)
{
    task_ctx *t = arg;
    unsigned char *buf = NULL;
    size_t buf_cap = 0;
    int rc = 0;
    for (size_t i = t->blob_from; i < t->blob_to && rc == 0; i++) {
        const unsigned char *blob = t->file + t->blob_off[i];
        const unsigned char *p = blob, *end = blob + t->blob_len[i];
        const unsigned char *zdata = NULL;
        size_t zlen = 0, raw_size = 0;
        while (p < end) {
            uint32_t f, w;
            p = pb_field(p, end, &f, &w);
            if (!p) { rc = -1; break; }
            if (w == 2) {
                uint64_t len;
                p = pb_varint(p, end, &len);
                if (!p || p + len > end) { rc = -1; break; }
                if (f == 1) { /* raw payload */
                    if (process_primitive_block(p, (size_t)len, t) != 0)
                        rc = -1;
                } else if (f == 3) { /* zlib payload */
                    zdata = p;
                    zlen = (size_t)len;
                }
                p += len;
            } else if (w == 0) {
                uint64_t v;
                p = pb_varint(p, end, &v);
                if (!p) { rc = -1; break; }
                if (f == 2)
                    raw_size = (size_t)v;
            } else {
                p = pb_skip(p, end, w);
                if (!p) { rc = -1; break; }
            }
        }
        if (rc == 0 && zdata) {
            size_t need = raw_size ? raw_size : zlen * 6 + 64;
            if (need > buf_cap) {
                unsigned char *nb = realloc(buf, need);
                if (!nb) { rc = -1; break; }
                buf = nb;
                buf_cap = need;
            }
            uLongf dlen = (uLongf)buf_cap;
            if (uncompress(buf, &dlen, zdata, (uLong)zlen) != Z_OK) {
                rc = -1;
                break;
            }
            if (process_primitive_block(buf, (size_t)dlen, t) != 0)
                rc = -1;
        }
    }
    free(buf);
    return (void *)(intptr_t)rc;
}

/* ------------------------------------------------------------- top level -- */

static int read_file(const char *path, unsigned char **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    unsigned char *b = malloc((size_t)sz);
    if (!b) {
        fclose(f);
        return -1;
    }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(b);
        return -1;
    }
    *buf = b;
    *len = (size_t)sz;
    return 0;
}

/* --- lake assembly (water multipolygon relations) ------------------------- */

typedef struct {
    int64_t node;
    int32_t way;
    int32_t seq;   /* insertion order: keeps the chaining deterministic */
    uint8_t end;   /* 0 = way starts here, 1 = way ends here */
    uint8_t used;
} endpoint_rec;

static int cmp_endpoint(const void *a, const void *b)
{
    const endpoint_rec *x = a, *y = b;
    if (x->node != y->node)
        return x->node < y->node ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

static size_t endpoint_lower_bound(const endpoint_rec *v, size_t n, int64_t node)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v[mid].node < node)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* True when the straight segment enters the extraction rectangle (Liang-Barsky). */
static int segment_hits_rect(double lat0, double lon0, double lat1, double lon1,
                             const double bbox[4])
{
    const double start[2] = {lat0, lon0};
    const double delta[2] = {lat1 - lat0, lon1 - lon0};
    const double lo[2] = {bbox[0], bbox[1]}, hi[2] = {bbox[2], bbox[3]};
    double t0 = 0.0, t1 = 1.0;
    for (int i = 0; i < 2; i++) {
        if (delta[i] == 0.0) {
            if (start[i] < lo[i] || start[i] > hi[i])
                return 0;
            continue;
        }
        double ta = (lo[i] - start[i]) / delta[i];
        double tb = (hi[i] - start[i]) / delta[i];
        if (ta > tb) {
            double tmp = ta;
            ta = tb;
            tb = tmp;
        }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1)
            return 0;
    }
    return 1;
}

/* Appends the lakes (and wide rivers) modelled as multipolygon relations. */
/* Walks the member ways of ONE relation into rings: ``loc[i]`` is the mways record
 * of member i and ``role_of`` its role in the relation. Keeping the members grouped
 * per relation is what stops the walk from chaining a lake to the river that leaves
 * it (they share nodes but belong to different relations), which used to leave the
 * ring open - or closed with a straight black band across the map. */
static int assemble_group(const pass1_out *m, const int32_t *loc, const uint8_t *role_of,
                          size_t n, const node_table *table, const double bbox[4],
                          mm_data *out)
{
    if (!n)
        return 0;
    endpoint_rec *ends = malloc(sizeof(endpoint_rec) * n * 2);
    uint8_t *used = calloc(n, 1);
    uint8_t *inside = calloc(n, 1);
    if (!ends || !used || !inside) {
        free(ends);
        free(used);
        free(inside);
        return -1;
    }
    /* Only outline ways that touch the extraction area take part, exactly like
     * the Python implementation (otherwise distant ways chain into rings that
     * span whole regions, e.g. the Ligurian coastline). */
    for (size_t i = 0; i < n; i++) {
        const way_rec *wr = &m->mways[loc[i]];
        for (int32_t r = 0; r < wr->ref_cnt; r++) {
            size_t slot;
            if (!table_find(table, m->mrefs[wr->ref_off + r], &slot))
                continue;
            double lat = table->lat_e5[slot] / MM_E5;
            double lon = table->lon_e5[slot] / MM_E5;
            if (lat >= bbox[0] && lat <= bbox[2] && lon >= bbox[1] && lon <= bbox[3]) {
                inside[i] = 1;
                break;
            }
        }
    }
    size_t n_end = 0;
    for (size_t i = 0; i < n; i++) {
        const way_rec *wr = &m->mways[loc[i]];
        if (wr->ref_cnt < 2 || !inside[i])
            continue;
        ends[n_end].node = m->mrefs[wr->ref_off];
        ends[n_end].way = (int32_t)i;
        ends[n_end].seq = (int32_t)n_end;
        ends[n_end].end = 0;
        ends[n_end].used = 0;
        n_end++;
        ends[n_end].node = m->mrefs[wr->ref_off + wr->ref_cnt - 1];
        ends[n_end].way = (int32_t)i;
        ends[n_end].seq = (int32_t)n_end;
        ends[n_end].end = 1;
        ends[n_end].used = 0;
        n_end++;
    }
    qsort(ends, n_end, sizeof(endpoint_rec), cmp_endpoint);

    for (size_t start = 0; start < n; start++) {
        const way_rec *wr = &m->mways[loc[start]];
        if (used[start] || wr->ref_cnt < 2 || !inside[start])
            continue;
        used[start] = 1;
        int role = role_of[loc[start]];
        int32_t ring_start = out->n_points;
        for (int32_t r = 0; r < wr->ref_cnt; r++) {
            size_t slot;
            if (table_find(table, m->mrefs[wr->ref_off + r], &slot)) {
                out->lat_e5[out->n_points] = table->lat_e5[slot];
                out->lon_e5[out->n_points] = (int32_t)table->lon_e5[slot];
                out->n_points++;
            }
        }
        int64_t head = m->mrefs[wr->ref_off];
        int64_t tail = m->mrefs[wr->ref_off + wr->ref_cnt - 1];
        while (tail != head) {
            size_t k = endpoint_lower_bound(ends, n_end, tail);
            int32_t nxt = -1, side = 0;
            for (; k < n_end && ends[k].node == tail; k++) {
                int32_t wi = ends[k].way;
                if (used[wi] || !inside[wi])
                    continue;
                nxt = wi;
                side = ends[k].end;
                break;
            }
            if (nxt < 0)
                break;
            used[nxt] = 1;
            const way_rec *nw = &m->mways[loc[nxt]];
            if (role_of[loc[nxt]] == 1)
                role = 1;
            if (side == 0) {
                for (int32_t r = 1; r < nw->ref_cnt; r++) {
                    size_t slot;
                    if (table_find(table, m->mrefs[nw->ref_off + r], &slot)) {
                        out->lat_e5[out->n_points] = table->lat_e5[slot];
                        out->lon_e5[out->n_points] = (int32_t)table->lon_e5[slot];
                        out->n_points++;
                    }
                }
                tail = m->mrefs[nw->ref_off + nw->ref_cnt - 1];
            } else {
                for (int32_t r = nw->ref_cnt - 2; r >= 0; r--) {
                    size_t slot;
                    if (table_find(table, m->mrefs[nw->ref_off + r], &slot)) {
                        out->lat_e5[out->n_points] = table->lat_e5[slot];
                        out->lon_e5[out->n_points] = (int32_t)table->lon_e5[slot];
                        out->n_points++;
                    }
                }
                tail = m->mrefs[nw->ref_off];
            }
        }
        int32_t cnt = out->n_points - ring_start;
        if (cnt < 4) {
            out->n_points = ring_start;
            continue;
        }
        mm_way *dst = &out->ways[out->n_ways++];
        dst->type = MM_WATER;
        dst->prio = (uint8_t)(role ? MM_WATER_RING_INNER : MM_WATER_RING_OUTER);
        dst->npts = cnt;
        dst->off = ring_start;
        /* "closed" means "safe to fill": a ring that really closes always is, and a
         * truncated one (the relation continues outside the extracted area) is when
         * the edge the rasteriser would add stays outside the rectangle. Otherwise
         * the ring is only stroked, or the fill would paint a band across the map. */
        int32_t last = out->n_points - 1;
        int closed = (out->lat_e5[ring_start] == out->lat_e5[last] &&
                      out->lon_e5[ring_start] == out->lon_e5[last]);
        if (!closed) {
            closed = !segment_hits_rect(out->lat_e5[last] / MM_E5, out->lon_e5[last] / MM_E5,
                                        out->lat_e5[ring_start] / MM_E5,
                                        out->lon_e5[ring_start] / MM_E5, bbox);
        }
        dst->closed = (uint8_t)closed;
    }
    free(ends);
    free(used);
    free(inside);
    return 0;
}

typedef struct {
    int64_t id;
    int32_t rec;
} id_rec;

static int cmp_id_rec(const void *a, const void *b)
{
    const id_rec *x = a, *y = b;
    return x->id < y->id ? -1 : (x->id > y->id ? 1 : 0);
}

/* Appends the lakes (and wide rivers) modelled as multipolygon relations, one
 * relation at a time (see assemble_group). */
static int assemble_rings(const pass1_out *m, const node_table *table, const double bbox[4],
                          mm_data *out)
{
    if (!m->n_rel || !m->mways_len)
        return 0;
    id_rec *by_id = malloc(sizeof(id_rec) * m->mways_len);
    int32_t *loc = malloc(sizeof(int32_t) * m->rel_ways_len);
    uint8_t *role_of = malloc(m->rel_ways_len);
    if (!by_id || !loc || !role_of) {
        free(by_id);
        free(loc);
        free(role_of);
        return -1;
    }
    for (size_t i = 0; i < m->mways_len; i++) {
        by_id[i].id = m->mway_ids[i];
        by_id[i].rec = (int32_t)i;
    }
    qsort(by_id, m->mways_len, sizeof(id_rec), cmp_id_rec);

    for (size_t r = 0; r < m->n_rel; r++) {
        size_t first = (size_t)m->rel_offsets[r];
        size_t last = (size_t)m->rel_offsets[r + 1];
        size_t n = 0;
        for (size_t i = first; i < last; i++) {
            int64_t wid = m->rel_ways[i];
            size_t lo = 0, hi = m->mways_len;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (by_id[mid].id < wid)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo >= m->mways_len || by_id[lo].id != wid)
                continue;               /* member way not collected (outside the area) */
            loc[n] = by_id[lo].rec;
            role_of[n] = m->rel_roles[i];
            n++;
        }
        if (assemble_group(m, loc, role_of, n, table, bbox, out) != 0) {
            free(by_id);
            free(loc);
            free(role_of);
            return -1;
        }
    }
    free(by_id);
    free(loc);
    free(role_of);
    return 0;
}

static int merge_pass1(pass1_out *dst, const pass1_out *src)
{
    if (src->ways_len) {
        int64_t ref_base = (int64_t)dst->refs_len;
        if (reserve((void **)&dst->ways, &dst->ways_cap, dst->ways_len + src->ways_len,
                    sizeof(way_rec)) != 0)
            return -1;
        memcpy(dst->ways + dst->ways_len, src->ways, sizeof(way_rec) * src->ways_len);
        /* per-thread reference arrays are concatenated, so the offsets stored
         * in the way records must be rebased onto the merged array */
        for (size_t i = 0; i < src->ways_len; i++)
            dst->ways[dst->ways_len + i].ref_off += ref_base;
        dst->ways_len += src->ways_len;
    }
    if (src->refs_len) {
        if (reserve((void **)&dst->refs, &dst->refs_cap, dst->refs_len + src->refs_len,
                    sizeof(int64_t)) != 0)
            return -1;
        memcpy(dst->refs + dst->refs_len, src->refs, sizeof(int64_t) * src->refs_len);
        dst->refs_len += src->refs_len;
    }
    if (src->mways_len) {
        int64_t ref_base = (int64_t)dst->mrefs_len;
        if (reserve((void **)&dst->mways, &dst->mways_cap, dst->mways_len + src->mways_len,
                    sizeof(way_rec)) != 0)
            return -1;
        if (reserve((void **)&dst->mway_ids, &dst->mway_ids_cap, dst->mways_len + src->mways_len,
                    sizeof(int64_t)) != 0)
            return -1;
        memcpy(dst->mways + dst->mways_len, src->mways, sizeof(way_rec) * src->mways_len);
        memcpy(dst->mway_ids + dst->mways_len, src->mway_ids, sizeof(int64_t) * src->mways_len);
        for (size_t i = 0; i < src->mways_len; i++)
            dst->mways[dst->mways_len + i].ref_off += ref_base;
        dst->mways_len += src->mways_len;
    }
    if (src->n_rel) {
        if (reserve((void **)&dst->rel_ways, &dst->rel_ways_cap,
                    dst->rel_ways_len + src->rel_ways_len, sizeof(int64_t)) != 0 ||
            reserve((void **)&dst->rel_roles, &dst->rel_roles_cap,
                    dst->rel_ways_len + src->rel_ways_len, 1) != 0 ||
            reserve((void **)&dst->rel_offsets, &dst->rel_offsets_cap,
                    dst->n_rel + src->n_rel + 1, sizeof(int64_t)) != 0)
            return -1;
        if (dst->n_rel == 0)
            dst->rel_offsets[0] = 0;
        int64_t base = (int64_t)dst->rel_ways_len;
        memcpy(dst->rel_ways + dst->rel_ways_len, src->rel_ways,
               sizeof(int64_t) * src->rel_ways_len);
        memcpy(dst->rel_roles + dst->rel_ways_len, src->rel_roles, src->rel_ways_len);
        for (size_t r = 0; r < src->n_rel; r++) {
            dst->rel_offsets[dst->n_rel + 1 + r] = base + src->rel_offsets[r + 1];
        }
        dst->n_rel += src->n_rel;
        dst->rel_ways_len += src->rel_ways_len;
    }
    if (src->mrefs_len) {
        if (reserve((void **)&dst->mrefs, &dst->mrefs_cap, dst->mrefs_len + src->mrefs_len,
                    sizeof(int64_t)) != 0)
            return -1;
        memcpy(dst->mrefs + dst->mrefs_len, src->mrefs, sizeof(int64_t) * src->mrefs_len);
        dst->mrefs_len += src->mrefs_len;
    }
    for (size_t i = 0; i < src->pl_len; i++) {
        const char *nm = src->pl_name_len[i] ? src->names + src->pl_name_off[i] : NULL;
        if (pass1_add_place(dst, src->pl_lat[i], src->pl_lon[i], src->pl_kind[i],
                            nm, src->pl_name_len[i], src->pl_pop[i]) != 0)
            return -1;
    }
    return 0;
}

int mm_pbf_extract(const char *path, const double filter_bbox[4], const double data_bbox[4],
                   mm_data *out, int threads)
{
    const double *bbox = filter_bbox;
    memset(out, 0, sizeof(*out));
    unsigned char *file = NULL;
    size_t file_len = 0;
    if (read_file(path, &file, &file_len) != 0)
        return -1;

    /* --- index the blobs --- */
    size_t cap = 4096, count = 0;
    size_t *offs = malloc(sizeof(size_t) * cap);
    uint32_t *lens = malloc(sizeof(uint32_t) * cap);
    if (!offs || !lens) {
        free(offs); free(lens); free(file);
        return -1;
    }
    size_t pos = 0;
    while (pos + 4 <= file_len) {
        uint32_t hdr_len = ((uint32_t)file[pos] << 24) | ((uint32_t)file[pos + 1] << 16) |
                           ((uint32_t)file[pos + 2] << 8) | (uint32_t)file[pos + 3];
        pos += 4;
        if (hdr_len == 0 || pos + hdr_len > file_len)
            break;
        const unsigned char *p = file + pos, *end = p + hdr_len;
        uint32_t datasize = 0;
        const char *type = NULL;
        size_t type_len = 0;
        while (p < end) {
            uint32_t f, w;
            p = pb_field(p, end, &f, &w);
            if (!p)
                break;
            if (w == 2) {
                uint64_t len;
                p = pb_varint(p, end, &len);
                if (!p || p + len > end)
                    break;
                if (f == 1) {
                    type = (const char *)p;
                    type_len = (size_t)len;
                }
                p += len;
            } else if (w == 0) {
                uint64_t v;
                p = pb_varint(p, end, &v);
                if (!p)
                    break;
                if (f == 3)
                    datasize = (uint32_t)v;
            } else {
                p = pb_skip(p, end, w);
                if (!p)
                    break;
            }
        }
        pos += hdr_len;
        if (pos + datasize > file_len)
            break;
        if (type && type_len == 7 && memcmp(type, "OSMData", 7) == 0) {
            if (count == cap) {
                cap *= 2;
                size_t *no = realloc(offs, sizeof(size_t) * cap);
                uint32_t *nl = realloc(lens, sizeof(uint32_t) * cap);
                if (!no || !nl) {
                    free(no ? no : offs);
                    free(nl ? nl : lens);
                    free(file);
                    return -1;
                }
                offs = no;
                lens = nl;
            }
            offs[count] = pos;
            lens[count] = datasize;
            count++;
        }
        pos += datasize;
    }

    if (threads < 1)
        threads = 1;
    if ((size_t)threads > count)
        threads = (int)(count ? count : 1);

    pass1_out merged;
    pass1_init(&merged);
    task_ctx *ctxs = calloc((size_t)threads, sizeof(task_ctx));
    pthread_t *tids = calloc((size_t)threads, sizeof(pthread_t));
    node_table table;
    memset(&table, 0, sizeof(table));
    int failed = 0;
    if (!ctxs || !tids)
        failed = 1;

    size_t per = count ? (count + (size_t)threads - 1) / (size_t)threads : 0;
    member_map shared_members;
    memset(&shared_members, 0, sizeof(shared_members));

    /* -------- pass 1: relations (water multipolygons) -------- */
    if (!failed) {
        for (int i = 0; i < threads; i++) {
            task_ctx *c = &ctxs[i];
            c->pass = 1;
            c->file = file;
            c->blob_off = offs;
            c->blob_len = lens;
            c->blob_count = count;
            c->blob_from = per * (size_t)i;
            c->blob_to = per * (size_t)(i + 1);
            if (c->blob_to > count)
                c->blob_to = count;
            if (c->blob_from > count)
                c->blob_from = count;
            memcpy(c->bbox, bbox, sizeof(double) * 4);
            pass1_init(&c->out);
            memset(&c->members, 0, sizeof(c->members));
        }
        for (int i = 0; i < threads; i++)
            pthread_create(&tids[i], NULL, worker, &ctxs[i]);
        for (int i = 0; i < threads; i++) {
            void *res = NULL;
            pthread_join(tids[i], &res);
            if ((intptr_t)res != 0)
                failed = 1;
        }
        for (int i = 0; i < threads; i++) {
            member_map *m = &ctxs[i].members;
            for (size_t k = 0; !failed && k < m->count; k++)
                if (m->used[k] && member_map_add(&shared_members, m->keys[k], m->roles[k]) != 0)
                    failed = 1;
            free(m->keys);
            free(m->roles);
            free(m->used);
            memset(m, 0, sizeof(*m));
            /* The relation groups themselves live in the task output too: they must
             * survive the pass (pass 2 re-initialises out), otherwise the ring
             * assembly finds no relation at all and the lakes disappear. */
            if (!failed && merge_pass1(&merged, &ctxs[i].out) != 0)
                failed = 1;
            pass1_free(&ctxs[i].out);
        }
    }

    /* -------- pass 2: ways (+ lake outline members) and places -------- */
    if (!failed) {
        for (int i = 0; i < threads; i++) {
            task_ctx *c = &ctxs[i];
            c->pass = 2;
            c->file = file;
            c->blob_off = offs;
            c->blob_len = lens;
            c->blob_count = count;
            c->blob_from = per * (size_t)i;
            c->blob_to = per * (size_t)(i + 1);
            if (c->blob_to > count)
                c->blob_to = count;
            if (c->blob_from > count)
                c->blob_from = count;
            memcpy(c->bbox, bbox, sizeof(double) * 4);
            pass1_init(&c->out);
            c->members = shared_members;   /* read-only in the workers */
        }
        for (int i = 0; i < threads; i++)
            pthread_create(&tids[i], NULL, worker, &ctxs[i]);
        for (int i = 0; i < threads; i++) {
            void *res = NULL;
            pthread_join(tids[i], &res);
            if ((intptr_t)res != 0)
                failed = 1;
        }
        for (int i = 0; i < threads; i++) {
            if (merge_pass1(&merged, &ctxs[i].out) != 0)
                failed = 1;
            pass1_free(&ctxs[i].out);
        }
    }

    /* node id table sized for the referenced nodes */
    if (!failed) {
        size_t n_refs = merged.refs_len + merged.mrefs_len;
        size_t tcap = 16;
        while (tcap < n_refs * 2 + 16)
            tcap *= 2;
        table.cap = tcap;
        table.keys = malloc(sizeof(int64_t) * tcap);
        table.used = calloc(tcap, 1);
        table.lat_e5 = malloc(sizeof(int32_t) * tcap);
        table.lon_e5 = malloc(sizeof(int64_t) * tcap);
        if (!table.keys || !table.used || !table.lat_e5 || !table.lon_e5)
            failed = 1;
        for (size_t i = 0; !failed && i < merged.refs_len; i++)
            table_insert(&table, merged.refs[i]);
        for (size_t i = 0; !failed && i < merged.mrefs_len; i++)
            table_insert(&table, merged.mrefs[i]);
    }

    /* -------- pass 3: node coordinates -------- */
    if (!failed) {
        for (int i = 0; i < threads; i++) {
            task_ctx *c = &ctxs[i];
            c->pass = 3;
            c->table = &table;
        }
        for (int i = 0; i < threads; i++)
            pthread_create(&tids[i], NULL, worker, &ctxs[i]);
        for (int i = 0; i < threads; i++) {
            void *res = NULL;
            pthread_join(tids[i], &res);
            if ((intptr_t)res != 0)
                failed = 1;
        }
    }

    /* assemble the final data */
    if (!failed) {
        memcpy(out->bbox, data_bbox, sizeof(double) * 4);
        size_t max_ways = merged.ways_len + merged.mways_len + 1;
        size_t max_points = merged.refs_len + merged.mrefs_len + 1;
        out->ways = malloc(sizeof(mm_way) * max_ways);
        out->lat_e5 = malloc(sizeof(int32_t) * max_points);
        out->lon_e5 = malloc(sizeof(int32_t) * max_points);
        if (!out->ways || !out->lat_e5 || !out->lon_e5)
            failed = 1;
    }
    if (!failed) {
        int32_t n_points = 0;
        for (size_t w = 0; w < merged.ways_len; w++) {
            const way_rec *wr = &merged.ways[w];
            int32_t start = n_points;
            for (int32_t r = 0; r < wr->ref_cnt; r++) {
                size_t slot;
                if (!table_find(&table, merged.refs[wr->ref_off + r], &slot))
                    continue;
                out->lat_e5[n_points] = table.lat_e5[slot];
                out->lon_e5[n_points] = (int32_t)table.lon_e5[slot];
                n_points++;
            }
            int32_t cnt = n_points - start;
            if (cnt < 2) {
                n_points = start;
                continue;
            }
            int inside = 0;
            for (int32_t j = start; j < start + cnt; j++) {
                double lat = out->lat_e5[j] / MM_E5, lon = out->lon_e5[j] / MM_E5;
                if (lat >= bbox[0] && lat <= bbox[2] && lon >= bbox[1] && lon <= bbox[3]) {
                    inside = 1;
                    break;
                }
            }
            if (!inside) {
                n_points = start;
                continue;
            }
            mm_way *dst = &out->ways[out->n_ways++];
            dst->type = (uint8_t)wr->type;
            dst->prio = (uint8_t)wr->prio;
            dst->npts = cnt;
            dst->off = start;
            dst->closed = (cnt >= 4 && out->lat_e5[start] == out->lat_e5[start + cnt - 1] &&
                           out->lon_e5[start] == out->lon_e5[start + cnt - 1]) ? 1 : 0;
        }
        out->n_points = n_points;
        if (assemble_rings(&merged, &table, filter_bbox, out) != 0)
            failed = 1;
        out->n_places = (int32_t)merged.pl_len;
        size_t np = merged.pl_len ? merged.pl_len : 1;
        out->place_lat_e5 = malloc(sizeof(int32_t) * np);
        out->place_lon_e5 = malloc(sizeof(int32_t) * np);
        out->place_kind = malloc(np);
        out->place_population = malloc(sizeof(int32_t) * np);
        out->name_off = malloc(sizeof(int32_t) * np);
        out->name_len = malloc(sizeof(uint16_t) * np);
        out->names = malloc(merged.names_len ? merged.names_len : 1);
        if (!out->place_lat_e5 || !out->place_lon_e5 || !out->place_kind ||
            !out->place_population || !out->name_off || !out->name_len || !out->names) {
            failed = 1;
        } else {
            memcpy(out->place_lat_e5, merged.pl_lat, sizeof(int32_t) * merged.pl_len);
            memcpy(out->place_lon_e5, merged.pl_lon, sizeof(int32_t) * merged.pl_len);
            memcpy(out->place_kind, merged.pl_kind, merged.pl_len);
            memcpy(out->place_population, merged.pl_pop, sizeof(int32_t) * merged.pl_len);
            memcpy(out->name_off, merged.pl_name_off, sizeof(int32_t) * merged.pl_len);
            memcpy(out->name_len, merged.pl_name_len, sizeof(uint16_t) * merged.pl_len);
            memcpy(out->names, merged.names, merged.names_len);
            out->names_len = (uint32_t)merged.names_len;
        }
    }

    free(table.keys);
    free(table.used);
    free(table.lat_e5);
    free(table.lon_e5);
    pass1_free(&merged);
    free(ctxs);
    free(tids);
    free(offs);
    free(lens);
    free(file);
    if (failed) {
        mm_data_free(out);
        return -1;
    }
    return 0;
}
