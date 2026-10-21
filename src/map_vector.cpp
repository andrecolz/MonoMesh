#include "map_vector.h"

#include <esp_heap_caps.h>
#include <SD_MMC.h>
#include <cstring>

static const char* TAG = "MonoMesh-MapVector";

namespace MonoMesh {

namespace {
void* psramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p && n <= 32 * 1024) p = malloc(n);
    return p;
}

// Allocator injection for mm_chunk_dir_build (libmapcore stays dependency free).
void* dirAlloc(void*, size_t n) { return psramAlloc(n); }
void dirDealloc(void*, void* p) { free(p); }
}  // namespace

const uint8_t* MapVector::preloadLookup(uint64_t off, size_t len) const {
    for (int i = 0; i < _preN; ++i) {
        const Preload& p = _pre[i];
        if (off >= p.off && off + len <= p.off + p.len) return p.data + (off - p.off);
    }
    return nullptr;
}

int64_t MapVector::ioRead(void* ctx, uint64_t off, void* buf, size_t len) {
    MapVector* self = (MapVector*)ctx;
    if (!self || !self->_file) return -1;
    if (len == 0) return 0;
    // Immutable index/label sections live in PSRAM after preloadIndexData():
    // the label pass reads POI entries and names straight from RAM instead of
    // walking the SD with three interleaved streams (the z11 frame killer).
    if (self->_preN) {
        const uint8_t* src = self->preloadLookup(off, len);
        if (src) {
            memcpy(buf, src, len);
            return (int64_t)len;
        }
    }
    // Small reads are served from the read-ahead window (plan D6: multi-KB
    // sequential SD access only); whole chunk payloads go straight through.
    if (self->_win && len <= kWinCap) {
        if (off >= self->_winOff && off + len <= self->_winOff + self->_winLen) {
            memcpy(buf, self->_win + (off - self->_winOff), len);
            return (int64_t)len;
        }
        uint64_t aligned = off & ~(uint64_t)255;
        if (self->_file.seek((uint32_t)aligned)) {
            size_t got = self->_file.read(self->_win, kWinCap);
            self->_winOff = aligned;
            self->_winLen = got;
            ++self->_winFills;
            if (off + len <= aligned + got) {
                memcpy(buf, self->_win + (off - aligned), len);
                return (int64_t)len;
            }
        }
    }
    if (!self->_file.seek((uint32_t)off)) return -1;
    size_t got = self->_file.read((uint8_t*)buf, len);
    return (int64_t)got;
}

bool MapVector::open(const char* path) {
    close();
    _file = SD_MMC.open(path, FILE_READ);
    if (!_file) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return false;
    }
    _io.read = ioRead;
    _io.ctx = this;
    _io.size = (uint64_t)_file.size();
    if (!_win) _win = (uint8_t*)psramAlloc(kWinCap);
    if (mm_vmap_open(&_map, &_io) != 0 || _map.n_bands == 0) {
        ESP_LOGW(TAG, "%s: not a vector container", path);
        close();
        return false;
    }
    _chunks.get_chunk = chunkTrampoline;
    _chunks.get_dir = dirTrampoline;
    _chunks.user = this;
    _open = true;

    const uint32_t cells = _map.n_bands > 0 ? _map.bands[0].nx * _map.bands[0].ny : 0;
    _lChunks = (LChunk*)psramAlloc(sizeof(LChunk) * (cells ? cells : 1));
    _lCells = _lChunks ? cells : 0;
    if (!_lChunks) {
        ESP_LOGW(TAG, "overview band index not allocated");
    } else {
        // psramAlloc does not zero: a partially loaded overview band must read
        // as "empty cell", never as a stale offset into the arena.
        memset(_lChunks, 0, sizeof(LChunk) * cells);
        _lDirs = (mm_chunk_dir*)psramAlloc(sizeof(mm_chunk_dir) * (cells ? cells : 1));
        _lDirNone = (uint8_t*)psramAlloc(cells ? cells : 1);
        _lDirBytesArr = (uint32_t*)psramAlloc(sizeof(uint32_t) * (cells ? cells : 1));
        _lDirStamp = (uint32_t*)psramAlloc(sizeof(uint32_t) * (cells ? cells : 1));
        if (_lDirs && _lDirNone && _lDirBytesArr && _lDirStamp) {
            memset(_lDirs, 0, sizeof(mm_chunk_dir) * cells);
            memset(_lDirNone, 0, cells);
            memset(_lDirBytesArr, 0, sizeof(uint32_t) * cells);
            memset(_lDirStamp, 0, sizeof(uint32_t) * cells);
        } else {
            ESP_LOGW(TAG, "overview directory bookkeeping not allocated");
            if (_lDirs) { free(_lDirs); _lDirs = nullptr; }
            if (_lDirNone) { free(_lDirNone); _lDirNone = nullptr; }
            if (_lDirBytesArr) { free(_lDirBytesArr); _lDirBytesArr = nullptr; }
            if (_lDirStamp) { free(_lDirStamp); _lDirStamp = nullptr; }
        }
    }

    ESP_LOGI(TAG, "%s: vector v%u style %u, bands %d (z%d..z%d), poi %u, %u KB",
             _map.name, _map.version, _map.style_version, _map.n_bands, zmin(), zmax(),
             _map.poi_count, (unsigned)(_io.size / 1024));
    for (int b = 0; b < _map.n_bands; ++b) {
        const mm_vmap_band* band = &_map.bands[b];
        ESP_LOGI(TAG, "  band %d: z%u-%u grid z%u %ux%u, %u cells, %u KB payload", b,
                 band->zmin, band->zmax, band->grid_z, band->nx, band->ny,
                 band->present_count, band->payload_bytes / 1024);
    }

    // One-time bulk preload of the immutable index + label sections (plan D6:
    // SD access must be multi-KB sequential).  Without this the label pass
    // interleaves three streams through the 8 KB window and every POI costs
    // seek + 8 KB read: ~1500 fills at z11, ~77000 at z7.
    if (preloadIndexData()) {
        ESP_LOGI(TAG, "index+labels preloaded: %u KB in %d regions",
                 (unsigned)(_preBytes / 1024), _preN);
    } else {
        ESP_LOGW(TAG, "index+label preload unavailable: scattered SD reads stay");
    }
    return true;
}

void MapVector::close() {
    if (_file) _file.close();
    for (int i = 0; i < _ringN; ++i) {
        if (_ring[i].data) free(_ring[i].data);
        mm_chunk_dir_free(&_ring[i].dir, dirDealloc, nullptr);
        _ring[i] = Slot();
    }
    _ringN = 0;
    _ringBytes = 0;
    if (_lData) {
        free(_lData);
        _lData = nullptr;
    }
    if (_scratch) {
        free(_scratch);
        _scratch = nullptr;
    }
    if (_win) {
        free(_win);
        _win = nullptr;
        _winLen = 0;
    }
    _lLen = _lCap = 0;
    _lFailed = false;
    if (_lChunks) {
        free(_lChunks);
        _lChunks = nullptr;
    }
    if (_lDirs) {
        for (uint32_t i = 0; i < _lCells; ++i)
            mm_chunk_dir_free(&_lDirs[i], dirDealloc, nullptr);
        free(_lDirs);
        _lDirs = nullptr;
    }
    if (_lDirNone) { free(_lDirNone); _lDirNone = nullptr; }
    if (_lDirBytesArr) { free(_lDirBytesArr); _lDirBytesArr = nullptr; }
    if (_lDirStamp) { free(_lDirStamp); _lDirStamp = nullptr; }
    _lDirBytes = 0;
    _lDirClock = 0;
    _lCells = 0;
    _dirBytes = 0;
    preloadFree();
    _map = mm_vmap{};
    _open = false;
    _reads = _hits = 0;
    _winFills = 0;
    _payloadBytes = 0;
}

void MapVector::preloadFree() {
    for (int i = 0; i < _preN; ++i) {
        if (_pre[i].data) free(_pre[i].data);
        _pre[i] = Preload();
    }
    _preN = 0;
    _preBytes = 0;
    _preFailed = false;
}

// Reads one immutable range into PSRAM.  The caller must not have published
// any preload region yet when this fails: preloadIndexData() frees everything
// and falls back to the windowed path, so a tight PSRAM never breaks labels.
bool MapVector::preloadAdd(uint64_t off, size_t len, uint8_t** dst, size_t* dstLen) {
    *dst = nullptr;
    *dstLen = 0;
    if (len == 0) return true;
    if (_preN >= kPreloadMax || _preBytes + len > kPreloadCap) return false;
    if (off > _io.size || len > _io.size - off) return false;
    uint8_t* buf = (uint8_t*)psramAlloc(len);
    if (!buf) return false;
    if (!_file.seek((uint32_t)off) || _file.read(buf, len) != len) {
        free(buf);
        return false;
    }
    Preload& p = _pre[_preN++];
    p.off = off;
    p.len = len;
    p.data = buf;
    _preBytes += len;
    *dst = buf;
    *dstLen = len;
    return true;
}

bool MapVector::preloadIndexData() {
    if (_preN) return true;
    if (_preFailed) return false;

    // Band index walks (presence popcount, row prefix): a few KB per band.
    for (int b = 0; b < _map.n_bands; ++b) {
        const mm_vmap_band* band = &_map.bands[b];
        uint8_t* buf = nullptr;
        size_t len = 0;
        size_t presence = ((size_t)band->nx * band->ny + 7) / 8;
        if (!preloadAdd(band->off_presence, presence, &buf, &len)) goto fail;
        if (!preloadAdd(band->off_prefix, ((size_t)band->ny + 1) * 4, &buf, &len)) goto fail;
    }

    if (_map.has_poigrid) {
        uint8_t* buf = nullptr;
        size_t len = 0;
        size_t presence = ((size_t)_map.pg_nx * _map.pg_ny + 7) / 8;
        size_t prefix = ((size_t)_map.pg_ny + 1) * 4;
        size_t index = (size_t)_map.pg_present * MM_POI_CELL_ENTRY_SIZE;
        size_t refs = (size_t)_map.pg_refs * MM_POI_REF_SIZE;
        if (!preloadAdd(_map.off_pg_presence, presence, &buf, &len)) goto fail;
        if (!preloadAdd(_map.off_pg_prefix, prefix, &buf, &len)) goto fail;
        if (!preloadAdd(_map.off_pg_index, index, &buf, &len)) goto fail;
        if (!preloadAdd(_map.off_pg_refs, refs, &buf, &len)) goto fail;
    }
    if (_map.poi_count) {
        uint8_t* buf = nullptr;
        size_t len = 0;
        if (!preloadAdd(_map.off_poi, (size_t)_map.poi_count * 20, &buf, &len)) goto fail;
    }
    if (_map.strings_bytes) {
        uint8_t* buf = nullptr;
        size_t len = 0;
        if (!preloadAdd(_map.off_strings, _map.strings_bytes, &buf, &len)) goto fail;
    }
    return _preN > 0;

fail:
    ESP_LOGW(TAG, "preload failed after %u KB: falling back to the window",
             (unsigned)(_preBytes / 1024));
    preloadFree();
    _preFailed = true;
    return false;
}

int MapVector::zmin() const {
    int z = 99;
    for (int b = 0; b < _map.n_bands; ++b) z = std::min(z, (int)_map.bands[b].zmin);
    return _map.n_bands ? z : 0;
}

int MapVector::zmax() const {
    int z = 0;
    for (int b = 0; b < _map.n_bands; ++b) z = std::max(z, (int)_map.bands[b].zmax);
    return z;
}

int MapVector::chunkTrampoline(void* user, int band, uint32_t cx, uint32_t cy,
                               const uint8_t** data, size_t* len, uint16_t* flags) {
    return ((MapVector*)user)->getChunk(band, cx, cy, data, len, flags);
}

// Band 0 (overview) is pinned whole: load every chunk once, keep the payloads
// decompressed in one arena so the z7-11 views never touch the LRU ring.  The
// chunks inflate straight into the arena (a single overview chunk can be
// ~850 KB decompressed on the reference container: no temp buffer is big
// enough), growing it and retrying when a chunk does not fit.
bool MapVector::loadBandL() {
    if (_lData) return true;
    if (_lFailed || !_lChunks || _lCells == 0) return false;
    const mm_vmap_band* band = &_map.bands[0];
    if (_lCap == 0) {
        _lCap = (size_t)band->payload_bytes + 256 * 1024;
        if (_lCap > kLBandMaxBytes) _lCap = kLBandMaxBytes;
        _lData = (uint8_t*)psramAlloc(_lCap);
        if (!_lData) {
            ESP_LOGW(TAG, "overview band arena not allocated (%u KB)", (unsigned)(_lCap / 1024));
            _lFailed = true;  // no retry storm: the cells stay empty
            return false;
        }
    }
    for (uint32_t cy = 0; cy < band->ny; ++cy) {
        for (uint32_t cx = 0; cx < band->nx; ++cx) {
            uint32_t idx = cy * band->nx + cx;
            _lChunks[idx] = LChunk{0, 0, 0};
            mm_vmap_cell cell;
            if (mm_vmap_cell_info(&_map, 0, band->x0 + cx, band->y0 + cy, &cell) != 0 ||
                cell.length == 0)
                continue;
            for (int attempt = 0; attempt < 8; ++attempt) {
                size_t got = 0;
                uint16_t flags = 0;
                if (mm_vmap_read_chunk(&_map, 0, band->x0 + cx, band->y0 + cy, _lData + _lLen,
                                       _lCap - _lLen, &got, &flags) == 0) {
                    _lChunks[idx] = LChunk{(uint32_t)_lLen, (uint32_t)got, flags};
                    _lLen += got;
                    ++_reads;
                    _payloadBytes += got;
                    break;
                }
                // arena full (or chunk bigger than the space left): grow and
                // retry.  +25% steps instead of doubling: the final cap stays
                // close to the real payload (the reference L band needs
                // 1.77 MB, doubling left a 2.67 MB arena).
                size_t want = _lCap + _lCap / 4;
                if (want <= _lCap) want = _lCap + 256 * 1024;
                if (want > kLBandMaxBytes) want = kLBandMaxBytes;
                if (want <= _lCap) {
                    ESP_LOGW(TAG, "overview band over the %u KB cap",
                             (unsigned)(kLBandMaxBytes / 1024));
                    _lFailed = true;
                    return false;
                }
                uint8_t* bigger = (uint8_t*)psramAlloc(want);
                if (!bigger) {
                    _lFailed = true;
                    return false;
                }
                memcpy(bigger, _lData, _lLen);
                free(_lData);
                _lData = bigger;
                _lCap = want;
            }
        }
    }
    ESP_LOGI(TAG, "overview band pinned: %u KB in %u cells", (unsigned)(_lLen / 1024), _lCells);
    return true;
}

void MapVector::ringEvict(size_t need) {
    while (_ringBytes + need > kRingBytes || _ringN >= kRingMaxEntries) {
        int victim = -1;
        for (int i = 0; i < _ringN; ++i)
            if (victim < 0 || _ring[i].stamp < _ring[victim].stamp) victim = i;
        if (victim < 0) return;
        free(_ring[victim].data);
        mm_chunk_dir_free(&_ring[victim].dir, dirDealloc, nullptr);
        _dirBytes -= _ring[victim].dirBytes;
        _ringBytes -= _ring[victim].len;
        _ring[victim] = _ring[_ringN - 1];
        --_ringN;
    }
}

int MapVector::dirTrampoline(void* user, int band, uint32_t cx, uint32_t cy,
                             const mm_chunk_dir** dir) {
    return ((MapVector*)user)->getDir(band, cx, cy, dir);
}

// Record directories are built lazily from the decompressed payload and cached
// with it (ring slot or pinned overview cell).  They are pure metadata: the
// payload stays the single source of truth.
int MapVector::getDir(int band, uint32_t cx, uint32_t cy, const mm_chunk_dir** dir) {
    if (!_open || band < 0 || band >= _map.n_bands) return -1;
    const mm_vmap_band* b = &_map.bands[band];
    if (cx < b->x0 || cy < b->y0 || cx >= b->x0 + b->nx || cy >= b->y0 + b->ny) return 1;

    // Pinned overview band: directories live in their own budget with LRU
    // eviction, so the hot cells (Milan: ~90k records) fit and strips stop
    // walking the whole payload.
    if (band == 0) {
        if (!loadBandL()) return -1;
        if (!_lDirs || !_lDirNone) return 1;
        uint32_t idx = (cy - b->y0) * b->nx + (cx - b->x0);
        const LChunk& c = _lChunks[idx];
        if (c.len == 0 || _lDirNone[idx]) return 1;
        mm_chunk_dir& d = _lDirs[idx];
        if (!d.pos) {
            int br = mm_chunk_dir_build(&d, _lData + c.offset, c.len, kLDirMaxRecords,
                                        dirAlloc, dirDealloc, nullptr);
            const size_t bytes = (size_t)d.n * MM_DIR_BYTES_PER_REC;
            if (br != 0 || d.n == 0 || bytes > kLDirBudget) {
                mm_chunk_dir_free(&d, dirDealloc, nullptr);
                _lDirNone[idx] = 1;
                return 1;
            }
            // make room: evict the least recently used other overview dir
            while (_lDirBytes + bytes > kLDirBudget) {
                int victim = -1;
                for (uint32_t i = 0; i < _lCells; ++i) {
                    if (i == idx || !_lDirs[i].pos) continue;
                    if (victim < 0 || _lDirStamp[i] < _lDirStamp[victim]) victim = (int)i;
                }
                if (victim < 0) {
                    mm_chunk_dir_free(&d, dirDealloc, nullptr);
                    _lDirNone[idx] = 1;
                    return 1;
                }
                mm_chunk_dir_free(&_lDirs[victim], dirDealloc, nullptr);
                _lDirBytes -= _lDirBytesArr[victim];
                _lDirBytesArr[victim] = 0;
            }
            _lDirBytesArr[idx] = (uint32_t)bytes;
            _lDirBytes += bytes;
        }
        _lDirStamp[idx] = ++_lDirClock;
        *dir = &d;
        return 0;
    }

    // The renderer asks for the payload first, but fetch it here too so a
    // directory can never refer to an evicted slot.
    const uint8_t* data = nullptr;
    size_t len = 0;
    uint16_t flags = 0;
    int rc = getChunk(band, cx, cy, &data, &len, &flags);
    if (rc != 0) return rc;
    for (int i = 0; i < _ringN; ++i) {
        Slot& s = _ring[i];
        if (s.band == band && s.cellX == cx && s.cellY == cy) {
            if (s.dirNone) return 1;   // decided once, never retried
            if (!s.dir.pos) {
                int br = mm_chunk_dir_build(&s.dir, s.data, s.len, kDirMaxRecords,
                                            dirAlloc, dirDealloc, nullptr);
                const size_t bytes = (size_t)s.dir.n * MM_DIR_BYTES_PER_REC;
                if (br != 0 || s.dir.n == 0 || _dirBytes + bytes > kRingDirBudget) {
                    // over the per-chunk cap, empty, or over the ring budget:
                    // drop it and use the legacy walk for this chunk for good
                    mm_chunk_dir_free(&s.dir, dirDealloc, nullptr);
                    s.dirNone = true;
                    return 1;
                }
                s.dirBytes = (uint32_t)bytes;
                _dirBytes += bytes;
            }
            *dir = &s.dir;
            return 0;
        }
    }
    return -1;
}

int MapVector::getChunk(int band, uint32_t cx, uint32_t cy, const uint8_t** data,
                        size_t* len, uint16_t* flags) {
    if (!_open || band < 0 || band >= _map.n_bands) return -1;
    const mm_vmap_band* b = &_map.bands[band];
    if (cx < b->x0 || cy < b->y0 || cx >= b->x0 + b->nx || cy >= b->y0 + b->ny) return 1;

    if (band == 0) {
        if (!loadBandL()) return -1;
        const LChunk& c = _lChunks[(cy - b->y0) * b->nx + (cx - b->x0)];
        if (c.len == 0) return 1;
        *data = _lData + c.offset;
        *len = c.len;
        *flags = c.flags;
        ++_hits;
        return 0;
    }

    ++_clock;
    for (int i = 0; i < _ringN; ++i) {
        Slot& s = _ring[i];
        if (s.band == band && s.cellX == cx && s.cellY == cy) {
            s.stamp = _clock;
            *data = s.data;
            *len = s.len;
            *flags = 0;
            ++_hits;
            return 0;
        }
    }
    // decompress into the PSRAM scratch, then keep an exact sized copy in the
    // ring (a static buffer would land in DRAM and overflow it)
    if (!_scratch) {
        _scratch = (uint8_t*)psramAlloc(kChunkMaxBytes);
        if (!_scratch) return -1;
    }
    uint8_t* tmp = _scratch;
    size_t got = 0;
    uint16_t cflags = 0;
    int rc = mm_vmap_read_chunk(&_map, band, cx, cy, tmp, kChunkMaxBytes, &got, &cflags);
    if (rc != 0) return rc > 0 ? 1 : -1;
    ringEvict(got);
    uint8_t* buf = (uint8_t*)psramAlloc(got ? got : 1);
    if (!buf) return -1;
    memcpy(buf, tmp, got);
    Slot& s = _ring[_ringN++];
    // The insertion slot can be a stale copy left by ringEvict's move-down:
    // its directory pointer (if any) is shared with the slot it was copied
    // from, which still owns it.  Drop the reference, never free it here, and
    // clear the per-slot directory bookkeeping too.
    s.dir = mm_chunk_dir{};
    s.dirBytes = 0;
    s.dirNone = false;
    s.data = buf;
    s.len = got;
    s.stamp = _clock;
    s.cellX = cx;
    s.cellY = cy;
    s.band = (uint8_t)band;
    _ringBytes += got;
    ++_reads;
    _payloadBytes += got;
    *data = buf;
    *len = got;
    *flags = cflags;
    return 0;
}

}  // namespace MonoMesh
