#include "mm_vmap.h"
#include "mm_inflate.h"
#include "mm_varint.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- helpers --- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t rd32s(const uint8_t *p)
{
    return (int32_t)rd32(p);
}

/* Exact read of n bytes; 0 on success, -1 on short/failed read. */
static int rd(const mm_vmap_io *io, uint64_t off, void *buf, size_t n)
{
    if (n == 0)
        return 0;
    if (off > io->size || n > io->size - off)
        return -1;
    int64_t got = io->read(io->ctx, off, buf, n);
    return (got == (int64_t)n) ? 0 : -1;
}

/* --------------------------------------------------------------- open ---- */

static int parse_band(mm_vmap *m, uint64_t sec_off, uint32_t sec_size, int band)
{
    uint8_t h[MM_VBAND_SIZE];
    if (sec_size < MM_VBAND_SIZE || rd(&m->io, sec_off, h, sizeof(h)) != 0)
        return -1;
    mm_vmap_band *b = &m->bands[band];
    b->grid_z = h[1];
    b->zmin = h[44];
    b->zmax = h[45];
    b->raw_default = (uint8_t)(rd16(h + 2) & MM_CHUNK_FLAG_RAW ? 1 : 0);
    b->x0 = rd32(h + 4);
    b->nx = rd32(h + 8);
    b->y0 = rd32(h + 12);
    b->ny = rd32(h + 16);
    uint32_t presence_rel = rd32(h + 20);
    uint32_t prefix_rel = rd32(h + 24);
    uint32_t index_rel = rd32(h + 28);
    uint64_t payload_off = rd32(h + 32);
    b->payload_bytes = rd32(h + 36);
    b->present_count = rd32(h + 40);

    /* grid sanity: a band grid must be small enough to index with 32-bit math */
    if (b->grid_z > 24 || b->nx == 0 || b->ny == 0 || b->nx > 65536 || b->ny > 65536)
        return -1;
    uint64_t cells = (uint64_t)b->nx * b->ny;
    if (cells > (1u << 24))
        return -1;
    uint64_t presence_bytes = (cells + 7) / 8;
    uint64_t prefix_bytes = ((uint64_t)b->ny + 1) * 4;
    uint64_t index_bytes = (uint64_t)b->present_count * MM_CHUNK_ENTRY_SIZE;

    /* offsets are relative to the section start and must lie inside it */
    if (presence_rel > sec_size || prefix_rel > sec_size || index_rel > sec_size)
        return -1;
    if (presence_bytes > sec_size - presence_rel)
        return -1;
    if (prefix_bytes > sec_size - prefix_rel)
        return -1;
    if (index_bytes > sec_size - index_rel)
        return -1;
    if (b->present_count > cells)
        return -1;
    if (payload_off > m->io.size ||
        b->payload_bytes > m->io.size - payload_off)
        return -1;

    b->off_presence = sec_off + presence_rel;
    b->off_prefix = sec_off + prefix_rel;
    b->off_index = sec_off + index_rel;
    b->off_payload = payload_off;
    return 0;
}

static int parse_zoom(mm_vmap *m, uint64_t sec_off, uint32_t sec_size, int slot)
{
    uint8_t h[64];
    if (sec_size < sizeof(h) || rd(&m->io, sec_off, h, sizeof(h)) != 0)
        return -1;
    mm_vmap_zoom *z = &m->zooms[slot];
    z->z = h[0];
    z->bits = h[1];
    z->x0 = rd32(h + 4);
    z->nx = rd32(h + 8);
    z->y0 = rd32(h + 12);
    z->ny = rd32(h + 16);
    uint32_t presence_rel = rd32(h + 20);
    uint32_t prefix_rel = rd32(h + 24);
    uint32_t index_rel = rd32(h + 28);
    uint64_t payload_off = rd32(h + 32);
    z->payload_bytes = rd32(h + 36);
    z->present_count = rd32(h + 40);

    if (z->z > 31 || z->bits > 2)
        return -1;
    if (z->nx == 0 || z->ny == 0 || z->nx > 65536 || z->ny > 65536)
        return -1;
    uint64_t cells = (uint64_t)z->nx * z->ny;
    uint64_t presence_bytes = (cells + 7) / 8;
    uint64_t prefix_bytes = ((uint64_t)z->ny + 1) * 4;
    uint64_t index_bytes = (uint64_t)z->present_count * 8; /* v1 TILE_ENTRY */
    if (presence_rel > sec_size || prefix_rel > sec_size || index_rel > sec_size)
        return -1;
    if (presence_bytes > sec_size - presence_rel || prefix_bytes > sec_size - prefix_rel ||
        index_bytes > sec_size - index_rel)
        return -1;
    if (z->present_count > cells)
        return -1;
    if (payload_off > m->io.size || z->payload_bytes > m->io.size - payload_off)
        return -1;
    z->off_presence = sec_off + presence_rel;
    z->off_prefix = sec_off + prefix_rel;
    z->off_index = sec_off + index_rel;
    z->off_payload = payload_off;
    return 0;
}

static int parse_poigrid(mm_vmap *m, uint64_t sec_off, uint32_t sec_size)
{
    uint8_t h[MM_POIGRID_SIZE];
    if (sec_size < sizeof(h) || rd(&m->io, sec_off, h, sizeof(h)) != 0)
        return -1;
    m->poigrid_z = h[0];
    m->pg_x0 = rd32(h + 4);
    m->pg_nx = rd32(h + 8);
    m->pg_y0 = rd32(h + 12);
    m->pg_ny = rd32(h + 16);
    uint32_t presence_rel = rd32(h + 20);
    uint32_t prefix_rel = rd32(h + 24);
    uint32_t index_rel = rd32(h + 28);
    uint32_t refs_rel = rd32(h + 32);
    uint32_t refs_bytes = rd32(h + 36);
    m->pg_present = rd32(h + 40);
    m->pg_refs = rd32(h + 44);

    if (m->poigrid_z > 24 || m->pg_nx == 0 || m->pg_ny == 0 ||
        m->pg_nx > 65536 || m->pg_ny > 65536)
        return -1;
    uint64_t cells = (uint64_t)m->pg_nx * m->pg_ny;
    uint64_t presence_bytes = (cells + 7) / 8;
    uint64_t prefix_bytes = ((uint64_t)m->pg_ny + 1) * 4;
    uint64_t index_bytes = (uint64_t)m->pg_present * MM_POI_CELL_ENTRY_SIZE;
    if (presence_rel > sec_size || prefix_rel > sec_size || index_rel > sec_size ||
        refs_rel > sec_size)
        return -1;
    if (presence_bytes > sec_size - presence_rel || prefix_bytes > sec_size - prefix_rel ||
        index_bytes > sec_size - index_rel)
        return -1;
    if (refs_bytes > sec_size - refs_rel)
        return -1;
    if (m->pg_present > cells)
        return -1;
    if ((uint64_t)m->pg_refs * MM_POI_REF_SIZE > refs_bytes)
        return -1;

    m->off_pg_presence = sec_off + presence_rel;
    m->off_pg_prefix = sec_off + prefix_rel;
    m->off_pg_index = sec_off + index_rel;
    m->off_pg_refs = sec_off + refs_rel;
    m->has_poigrid = 1;
    return 0;
}

int mm_vmap_open(mm_vmap *m, const mm_vmap_io *io)
{
    if (!m || !io || !io->read || io->size < 128)
        return -1;
    memset(m, 0, sizeof(*m));
    m->io = *io;

    uint8_t h[128];
    if (rd(io, 0, h, sizeof(h)) != 0)
        return -1;
    if (memcmp(h, MM_VMAP_MAGIC, 8) != 0)
        return -1;
    m->version = rd16(h + 8);
    uint16_t header_size = rd16(h + 10);
    m->flags = rd32(h + 12);
    uint32_t tile_px = rd32(h + 16);
    uint8_t payload_bits = h[20];
    uint8_t zoom_count = h[21];
    uint16_t section_count = rd16(h + 22);
    uint32_t table_off = rd32(h + 24);
    uint32_t file_size = rd32(h + 28);

    if (m->version != 1 && m->version != 2)
        return -1;
    if (header_size != 128)
        return -1;
    if (tile_px != 256)
        return -1;
    if (payload_bits > 2)
        return -1;
    if (zoom_count > MM_VMAP_MAX_ZOOMS || section_count > 512)
        return -1;
    if (file_size > io->size)
        return -1;

    /* style version rides in the flag word's upper half (plan D3) */
    m->style_version = (uint16_t)(m->flags >> 16);
    if (m->version >= 2 && m->style_version != MM_STYLE_VERSION)
        return -1;

    for (int i = 0; i < 4; i++)
        m->bbox_e7[i] = rd32s(h + 32 + i * 4);
    memcpy(m->name, h + 48, MM_VMAP_NAME_LEN);
    m->name[MM_VMAP_NAME_LEN] = '\0';
    memcpy(m->attribution, h + 80, MM_VMAP_ATTRIB_LEN);
    m->attribution[MM_VMAP_ATTRIB_LEN] = '\0';

    uint64_t table_end = (uint64_t)table_off + (uint64_t)section_count * 16;
    if (table_off < 128 || table_end > io->size)
        return -1;

    uint8_t ent[16];
    for (int i = 0; i < section_count; i++) {
        if (rd(io, (uint64_t)table_off + (uint64_t)i * 16, ent, sizeof(ent)) != 0)
            return -1;
        uint32_t type = rd32(ent);
        uint64_t off = rd32(ent + 4);
        uint64_t size = rd32(ent + 8);
        uint32_t aux = rd32(ent + 12);
        if (off > io->size || size > io->size - off)
            return -1;
        switch (type) {
        case MM_SEC_VBAND: {
            if (m->version < 2 || m->n_bands >= MM_VMAP_MAX_BANDS)
                return -1;
            if (parse_band(m, off, (uint32_t)size, m->n_bands) != 0)
                return -1;
            (void)aux;
            m->n_bands++;
            break;
        }
        case MM_SEC_ZOOM: {
            if (m->n_zooms >= MM_VMAP_MAX_ZOOMS)
                return -1;
            if (parse_zoom(m, off, (uint32_t)size, m->n_zooms) != 0)
                return -1;
            m->n_zooms++;
            break;
        }
        case MM_SEC_POI: {
            /* section payload: u32 count + count * 20 bytes */
            uint8_t cnt[4];
            if (size < 4 || rd(&m->io, off, cnt, 4) != 0)
                return -1;
            uint32_t n = rd32(cnt);
            if ((uint64_t)n * 20 + 4 > size)
                return -1;
            m->off_poi = off + 4;
            m->poi_count = n;
            break;
        }
        case MM_SEC_STRINGS:
            m->off_strings = off;
            m->strings_bytes = (uint32_t)size;
            break;
        case MM_SEC_POIGRID:
            if (m->version < 2)
                return -1;
            if (parse_poigrid(m, off, (uint32_t)size) != 0)
                return -1;
            break;
        case MM_SEC_PREVIEW:
        default:
            /* unknown sections are ignored on purpose: that is how the format
             * grows without breaking older readers */
            break;
        }
    }

    /* a vector container must carry bands, a raster one zooms */
    if (m->n_bands == 0 && m->n_zooms == 0)
        return -1;
    return 0;
}

/* --------------------------------------------------------------- cells --- */

static int cell_lookup(const mm_vmap_io *io, uint64_t off_presence,
                       uint64_t off_prefix, uint64_t off_index,
                       uint32_t nx, uint32_t ny, uint32_t x0, uint32_t y0,
                       uint32_t cx, uint32_t cy, uint32_t entry_size,
                       uint32_t present_count, mm_vmap_cell *out)
{
    if (cx < x0 || cy < y0)
        return 1;
    uint32_t lx = cx - x0, ly = cy - y0;
    if (lx >= nx || ly >= ny)
        return 1;

    uint64_t bit = (uint64_t)ly * nx + lx;
    uint8_t byte;
    if (rd(io, off_presence + (bit >> 3), &byte, 1) != 0)
        return -1;
    if (!(byte & (0x80u >> (bit & 7))))
        return 1;

    /* row prefix gives the rank of the first present cell of this row */
    uint8_t p0[4], p1[4];
    if (rd(io, off_prefix + (uint64_t)ly * 4, p0, 4) != 0 ||
        rd(io, off_prefix + (uint64_t)(ly + 1) * 4, p1, 4) != 0)
        return -1;
    uint32_t row_start = rd32(p0), row_end = rd32(p1);
    /* untrusted input: a corrupt row prefix must not index past the entry
     * array (present_count is validated against the index size at open) */
    if (row_end < row_start || row_end > present_count)
        return -1;

    /* rank within the row: popcount of the row's presence bits before lx */
    uint32_t rank = 0;
    uint64_t row_bit0 = (uint64_t)ly * nx;
    uint64_t bit_end = row_bit0 + lx;
    uint64_t byte0 = row_bit0 >> 3, byte1 = bit_end >> 3;
    for (uint64_t by = byte0; by <= byte1; by++) {
        uint8_t b;
        if (rd(io, off_presence + by, &b, 1) != 0)
            return -1;
        for (int k = 0; k < 8; k++) {
            uint64_t cur = by * 8 + (uint64_t)k;
            if (cur < row_bit0)
                continue;
            if (cur >= bit_end)
                break;
            if (b & (0x80u >> k))
                rank++;
        }
    }
    uint64_t slot = (uint64_t)row_start + rank;
    if (slot >= row_end)
        return -1;

    uint8_t e[MM_CHUNK_ENTRY_SIZE];
    uint32_t esz = entry_size > sizeof(e) ? sizeof(e) : entry_size;
    if (rd(io, off_index + slot * entry_size, e, esz) != 0)
        return -1;
    out->offset = rd32(e);
    out->length = rd32(e + 4);
    out->flags = rd16(e + 8);
    return 0;
}

int mm_vmap_cell_info(const mm_vmap *m, int band, uint32_t cx, uint32_t cy,
                 mm_vmap_cell *out)
{
    if (band < 0 || band >= m->n_bands)
        return -1;
    const mm_vmap_band *b = &m->bands[band];
    return cell_lookup(&m->io, b->off_presence, b->off_prefix, b->off_index,
                       b->nx, b->ny, b->x0, b->y0, cx, cy, MM_CHUNK_ENTRY_SIZE,
                       b->present_count, out);
}

int mm_vmap_read_chunk(mm_vmap *m, int band, uint32_t cx, uint32_t cy,
                       uint8_t *dst, size_t dst_cap, size_t *out_len,
                       uint16_t *chunk_flags)
{
    mm_vmap_cell cell;
    int rc = mm_vmap_cell_info(m, band, cx, cy, &cell);
    if (rc != 0)
        return rc;
    if (cell.length == 0)
        return 1;
    if (cell.offset > m->io.size || cell.length > m->io.size - cell.offset)
        return -1;
    if (chunk_flags)
        *chunk_flags = cell.flags;
    if (cell.flags & MM_CHUNK_FLAG_RAW) {
        if (cell.length > dst_cap)
            return -1;
        if (rd(&m->io, cell.offset, dst, cell.length) != 0)
            return -1;
        if (out_len)
            *out_len = cell.length;
        return 0;
    }
    /* compressed chunk: one zlib stream inflated in one shot */
    uint8_t *tmp = (uint8_t *)malloc(cell.length);
    if (!tmp)
        return -1;
    if (rd(&m->io, cell.offset, tmp, cell.length) != 0) {
        free(tmp);
        return -1;
    }
    mm_inflate_ctx *ctx = (mm_inflate_ctx *)malloc(sizeof(mm_inflate_ctx));
    if (!ctx) {
        free(tmp);
        return -1;
    }
    int irc = -1;
    size_t produced = 0;
    if (mm_inflate_init(ctx) == 0) {
        irc = mm_inflate(ctx, tmp, cell.length, dst, dst_cap, &produced);
        mm_inflate_end(ctx);
    }
    free(ctx);
    free(tmp);
    if (irc != 0)
        return -1;
    if (out_len)
        *out_len = produced;
    return 0;
}

/* ----------------------------------------------------------------- POI --- */

int mm_vmap_poi_get(const mm_vmap *m, uint32_t index, mm_vmap_poi *out)
{
    if (index >= m->poi_count)
        return -1;
    uint8_t e[20];
    if (rd(&m->io, m->off_poi + (uint64_t)index * 20, e, sizeof(e)) != 0)
        return -1;
    out->lat_e7 = rd32s(e);
    out->lon_e7 = rd32s(e + 4);
    out->kind = e[8];
    out->population = (int32_t)rd32(e + 12);
    out->name_off = rd32(e + 16);
    out->name_len = rd16(e + 10);
    return 0;
}

int mm_vmap_poi_name(const mm_vmap *m, const mm_vmap_poi *p, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return -1;
    buf[0] = '\0';
    if (p->name_len == 0)
        return 0;
    if ((uint64_t)p->name_off + p->name_len > m->strings_bytes)
        return -1;
    size_t n = p->name_len;
    if (n >= cap)
        n = cap - 1;
    if (rd(&m->io, m->off_strings + p->name_off, buf, n) != 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

int mm_vmap_poi_foreach(const mm_vmap *m, uint32_t tx0, uint32_t ty0,
                        uint32_t tx1, uint32_t ty1, mm_vmap_poi_cb cb, void *user)
{
    if (!m->has_poigrid || !cb)
        return -1;
    if (tx1 < tx0 || ty1 < ty0)
        return 0;
    if (tx0 < m->pg_x0) tx0 = m->pg_x0;
    if (ty0 < m->pg_y0) ty0 = m->pg_y0;
    if (tx1 >= m->pg_x0 + m->pg_nx) tx1 = m->pg_x0 + m->pg_nx - 1;
    if (ty1 >= m->pg_y0 + m->pg_ny) ty1 = m->pg_y0 + m->pg_ny - 1;

    for (uint32_t cy = ty0; cy <= ty1; cy++) {
        for (uint32_t cx = tx0; cx <= tx1; cx++) {
            mm_vmap_cell cell;
            int rc = cell_lookup(&m->io, m->off_pg_presence, m->off_pg_prefix,
                                 m->off_pg_index, m->pg_nx, m->pg_ny, m->pg_x0,
                                 m->pg_y0, cx, cy, MM_POI_CELL_ENTRY_SIZE,
                                 m->pg_present, &cell);
            if (rc != 0) {
                if (rc < 0)
                    return -1;
                continue;
            }
            uint32_t first = (uint32_t)cell.offset; /* ref range in the refs array */
            uint32_t count = cell.length;
            if ((uint64_t)first + count > m->pg_refs)
                return -1;
            for (uint32_t i = 0; i < count; i++) {
                uint8_t r[MM_POI_REF_SIZE];
                if (rd(&m->io, m->off_pg_refs + ((uint64_t)first + i) * MM_POI_REF_SIZE,
                       r, sizeof(r)) != 0)
                    return -1;
                uint32_t poi = rd32(r);
                int32_t score = rd32s(r + 4);
                if (poi >= m->poi_count)
                    continue; /* defensive: a stale ref is skipped, not fatal */
                cb(user, poi, score);
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------- chunk record decode - */

void mm_chunk_reader_init(mm_chunk_reader *r, const uint8_t *data, size_t len)
{
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->remaining = 0;
    r->cls = 0;
    r->flags = 0;
    r->x = r->y = 0;
    r->error = 0;
}

int mm_chunk_next_record(mm_chunk_reader *r)
{
    if (r->error)
        return -1;
    if (r->remaining > 0) {
        /* skip the unread points of the current record */
        while (r->remaining > 0) {
            if (mm_chunk_next_point(r) < 0)
                return -1;
        }
    }
    if (r->pos >= r->len)
        return 1;
    if (r->len - r->pos < 2) {
        r->error = 1;
        return -1;
    }
    uint8_t cf = r->data[r->pos++];
    r->cls = (uint8_t)(cf & 0x1f);
    r->flags = (uint8_t)(cf >> 5);
    uint8_t hint = r->data[r->pos++];
    uint32_t npts;
    if (hint == MM_REC_NPTS_ESCAPE) {
        int n = mm_varint_get(r->data + r->pos, r->len - r->pos, &npts);
        if (n <= 0) {
            r->error = 1;
            return -1;
        }
        r->pos += (size_t)n;
    } else {
        npts = (uint32_t)hint + 2;
    }
    if (r->cls >= MM_CLS_COUNT || npts < 2 || npts > (1u << 20)) {
        r->error = 1;
        return -1;
    }
    if (r->len - r->pos < 4) {
        r->error = 1;
        return -1;
    }
    r->x = (int16_t)(r->data[r->pos] | (r->data[r->pos + 1] << 8));
    r->y = (int16_t)(r->data[r->pos + 2] | (r->data[r->pos + 3] << 8));
    r->pos += 4;
    if (r->x < MM_VMAP_COORD_MIN || r->x > MM_VMAP_COORD_MAX ||
        r->y < MM_VMAP_COORD_MIN || r->y > MM_VMAP_COORD_MAX) {
        r->error = 1;
        return -1;
    }
    r->remaining = npts - 1;
    return 0;
}

/* -------------------------------------------------- record directory ----- */

void mm_chunk_dir_free(mm_chunk_dir *dir, mm_dir_free_fn dealloc, void *ctx)
{
    if (!dir)
        return;
    if (dealloc) {
        if (dir->pos)
            dealloc(ctx, dir->pos);
        if (dir->meta)
            dealloc(ctx, dir->meta);
    }
    dir->pos = NULL;
    dir->meta = NULL;
    dir->n = dir->cap = 0;
}

/* floor/ceil of a 1/16 px bound in 2-grid-px units, clamped to the cell so the
 * encoded box always contains the true one (over-inclusion is safe). */
static uint8_t dir_bound_lo(int32_t v)
{
    int32_t g = v / MM_DIR_BBOX_UNIT;              /* truncation */
    if (v < 0 && g * MM_DIR_BBOX_UNIT != v) g--;   /* floor */
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    return (uint8_t)g;
}

static uint8_t dir_bound_hi(int32_t v)
{
    int32_t g = v / MM_DIR_BBOX_UNIT;
    if (v > 0 && g * MM_DIR_BBOX_UNIT != v) g++;   /* ceil */
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    return (uint8_t)g;
}

int mm_chunk_dir_build(mm_chunk_dir *dir, const uint8_t *data, size_t len,
                       uint32_t max_records, mm_dir_alloc_fn alloc,
                       mm_dir_free_fn dealloc, void *ctx)
{
    if (!dir)
        return -1;
    dir->pos = NULL;
    dir->meta = NULL;
    dir->n = dir->cap = 0;
    if (!alloc || !dealloc)
        return -1;
    if (!data || len == 0)
        return 0;
    if (max_records == 0)
        return 1;

    mm_chunk_reader r;
    mm_chunk_reader_init(&r, data, len);
    for (;;) {
        uint32_t pos = (uint32_t)r.pos;
        int rr = mm_chunk_next_record(&r);
        if (rr != 0)
            break; /* 1 = end of chunk, -1 = corrupt tail */
        uint8_t cls = r.cls;
        uint8_t flags = r.flags;
        int32_t minx = r.x, maxx = r.x, miny = r.y, maxy = r.y;
        for (;;) {
            int pp = mm_chunk_next_point(&r);
            if (pp <= 0)
                break;
            if (r.x < minx) minx = r.x;
            if (r.x > maxx) maxx = r.x;
            if (r.y < miny) miny = r.y;
            if (r.y > maxy) maxy = r.y;
        }
        if (r.error)
            break; /* drop the corrupt record and stop */
        if (dir->n >= max_records) {
            mm_chunk_dir_free(dir, dealloc, ctx);
            return 1; /* over the cap: the caller uses the legacy walk */
        }
        if (dir->n == dir->cap) {
            uint32_t cap = dir->cap ? dir->cap * 2 : 64;
            if (cap > max_records)
                cap = max_records;
            uint32_t *npos = (uint32_t *)alloc(ctx, sizeof(uint32_t) * (size_t)cap);
            uint8_t *nmeta =
                (uint8_t *)alloc(ctx, MM_DIR_META_STRIDE * (size_t)cap);
            if (!npos || !nmeta) {
                if (npos) dealloc(ctx, npos);
                if (nmeta) dealloc(ctx, nmeta);
                mm_chunk_dir_free(dir, dealloc, ctx);
                return -1;
            }
            if (dir->n) {
                memcpy(npos, dir->pos, sizeof(uint32_t) * (size_t)dir->n);
                memcpy(nmeta, dir->meta, MM_DIR_META_STRIDE * (size_t)dir->n);
            }
            if (dir->pos) dealloc(ctx, dir->pos);
            if (dir->meta) dealloc(ctx, dir->meta);
            dir->pos = npos;
            dir->meta = nmeta;
            dir->cap = cap;
        }
        uint8_t *m = &dir->meta[MM_DIR_META_STRIDE * (size_t)dir->n];
        dir->pos[dir->n] = pos;
        m[0] = cls;
        m[1] = flags;
        m[2] = dir_bound_lo(minx);
        m[3] = dir_bound_lo(miny);
        m[4] = dir_bound_hi(maxx);
        m[5] = dir_bound_hi(maxy);
        dir->n++;
    }
    return 0;
}

int mm_chunk_next_point(mm_chunk_reader *r)
{
    if (r->error)
        return -1;
    if (r->remaining == 0)
        return 0;
    uint32_t dx, dy;
    int n = mm_varint_get(r->data + r->pos, r->len - r->pos, &dx);
    if (n <= 0) {
        r->error = 1;
        return -1;
    }
    r->pos += (size_t)n;
    n = mm_varint_get(r->data + r->pos, r->len - r->pos, &dy);
    if (n <= 0) {
        r->error = 1;
        return -1;
    }
    r->pos += (size_t)n;
    /* accumulate with wrap-around-free bounds: a corrupt delta stream must not
     * overflow the coordinate (undefined behaviour) nor fool the rasteriser */
    int32_t nx = (int32_t)((uint32_t)r->x + mm_zigzag_dec(dx));
    int32_t ny = (int32_t)((uint32_t)r->y + mm_zigzag_dec(dy));
    if (nx < MM_VMAP_COORD_MIN || nx > MM_VMAP_COORD_MAX ||
        ny < MM_VMAP_COORD_MIN || ny > MM_VMAP_COORD_MAX) {
        r->error = 1;
        return -1;
    }
    r->x = nx;
    r->y = ny;
    r->remaining--;
    return 1;
}
