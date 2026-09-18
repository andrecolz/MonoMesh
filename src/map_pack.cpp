#include "map_pack.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <esp_heap_caps.h>
#include <SD_MMC.h>

// Inflate on the device. The ESP32-S3 ROM ships miniz (tinfl) and Arduino's
// include paths expose the header as <esp32s3/rom/miniz.h>; the generic name is
// used when a core provides it, and the prototype is declared by hand otherwise
// (the symbol is in the S3 ROM linker script).
#if defined(__has_include)
#  if __has_include(<esp32s3/rom/miniz.h>)
#    include <esp32s3/rom/miniz.h>
#  elif __has_include(<miniz.h>)
#    include <miniz.h>
#  else
#    define MONOMESH_ROM_TINFL 1
#  endif
#else
#  define MONOMESH_ROM_TINFL 1
#endif

#ifdef MONOMESH_ROM_TINFL
extern "C" size_t tinfl_decompress_mem_to_mem(void* pOut_buf, size_t out_buf_len,
                                              const void* pSrc_buf, size_t src_buf_len, int flags);
enum { TINFL_FLAG_PARSE_ZLIB_HEADER = 1 };
#define TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ((size_t)(-1))
#endif

static const char* TAG = "MonoMesh-MapPack";

namespace MonoMesh {

namespace {
constexpr char kMagic[8] = {'M', 'O', 'N', 'O', 'M', 'M', 'A', 'P'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 128;
constexpr size_t kSectionEntrySize = 16;
constexpr size_t kZoomHeaderSize = 64;
constexpr size_t kTileEntrySize = 8;
constexpr uint32_t kSecZoom = 1;
constexpr uint16_t kTileFlagBlank = 1 << 0;

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

void* psramAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p && n <= 32 * 1024) p = malloc(n);
    return p;
}

int popcountRange(const uint8_t* bits, uint32_t from, uint32_t to) {
    int count = 0;
    uint32_t i = from;
    while (i < to && (i & 7)) {
        if (bits[i >> 3] & (0x80 >> (i & 7))) ++count;
        ++i;
    }
    while (i + 32 <= to) {
        uint32_t word;
        memcpy(&word, bits + (i >> 3), 4);
        count += __builtin_popcount(__builtin_bswap32(word));
        i += 32;
    }
    while (i < to) {
        if (bits[i >> 3] & (0x80 >> (i & 7))) ++count;
        ++i;
    }
    return count;
}
} // namespace

MapPack::~MapPack() {
    close();
}

void MapPack::setError(const char* msg) {
    _error = msg ? msg : "";
    if (!_error.empty()) ESP_LOGW(TAG, "%s", _error.c_str());
}

void MapPack::releaseIndex() {
    if (_presence) { free(_presence); _presence = nullptr; }
    if (_prefix) { free(_prefix); _prefix = nullptr; }
    if (_index) { free(_index); _index = nullptr; }
    _indexCount = 0;
    _sel = nullptr;
}

void MapPack::close() {
    releaseIndex();
    if (_scratch) { free(_scratch); _scratch = nullptr; _scratchCap = 0; }
    if (_inflate) { free(_inflate); _inflate = nullptr; }
    if (_file) _file.close();
    _zooms.clear();
    _selBits = 1;
}

bool MapPack::open(const char* path) {
    close();
    _path = path ? path : "";
    _error.clear();
    _file = SD_MMC.open(path, FILE_READ);
    if (!_file) {
        setError("container could not be opened");
        _path.clear();
        return false;
    }

    uint8_t head[kHeaderSize];
    if (_file.read(head, sizeof(head)) != (int)sizeof(head) || memcmp(head, kMagic, sizeof(kMagic)) != 0) {
        setError("invalid .mmap header");
        close();
        return false;
    }
    const uint16_t version = rd16(head + 8);
    const uint32_t flags = rd32(head + 12);
    _tilePx = rd16(head + 16);
    _bits = head[20];
    const uint8_t zoomCount = head[21];
    const uint16_t sectionCount = rd16(head + 22);
    const uint32_t tableOff = rd32(head + 24);
    const uint32_t fileSize = rd32(head + 28);
    if (version != kVersion || _bits < 1 || _bits > 2 || _tilePx == 0) {
        setError("unsupported .mmap version/format");
        close();
        return false;
    }
    if (fileSize != 0 && (size_t)fileSize > _file.size() + 4096) {
        ESP_LOGW(TAG, "declared size %u, file %u", (unsigned)fileSize, (unsigned)_file.size());
    }
    _dark = (flags & (1u << 4)) != 0;
    char name[33] = {0};
    char attr[49] = {0};
    memcpy(name, head + 48, 32);
    memcpy(attr, head + 80, 48);
    _name = name;
    _attribution = attr;

    // Validate before allocating: a corrupt header must not turn sectionCount into an allocation
    // of up to ~1 MB (std::bad_alloc on a crash-averse device).
    if (sectionCount == 0 || sectionCount > 64) {
        setError("invalid section table");
        close();
        return false;
    }
    std::vector<uint8_t> table((size_t)sectionCount * kSectionEntrySize);
    if (!_file.seek(tableOff) || _file.read(table.data(), table.size()) != (int)table.size()) {
        setError("section table unreadable");
        close();
        return false;
    }

    uint8_t zhead[kZoomHeaderSize];
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const uint8_t* e = table.data() + (size_t)i * kSectionEntrySize;
        const uint32_t type = rd32(e);
        const uint32_t off = rd32(e + 4);
        const uint32_t size = rd32(e + 8);
        if (type != kSecZoom) continue;
        if (size < kZoomHeaderSize || !_file.seek(off) ||
            _file.read(zhead, sizeof(zhead)) != (int)sizeof(zhead)) {
            setError("zoom section unreadable");
            close();
            return false;
        }
        ZoomRec rec;
        rec.info.z = zhead[0];
        rec.info.bits = zhead[1] ? zhead[1] : _bits;
        rec.info.x0 = rd32(zhead + 4);
        rec.info.nx = rd32(zhead + 8);
        rec.info.y0 = rd32(zhead + 12);
        rec.info.ny = rd32(zhead + 16);
        rec.offPresence = off + rd32(zhead + 20);
        rec.offPrefix = off + rd32(zhead + 24);
        rec.offIndex = off + rd32(zhead + 28);
        rec.offPayload = rd32(zhead + 32);
        rec.info.payloadBytes = rd32(zhead + 36);
        rec.info.present = rd32(zhead + 40);
        if (rec.info.nx == 0 || rec.info.ny == 0) continue;
        _zooms.push_back(rec);
    }
    if (_zooms.empty()) {
        setError("no zoom levels in the container");
        close();
        return false;
    }
    std::sort(_zooms.begin(), _zooms.end(),
              [](const ZoomRec& a, const ZoomRec& b) { return a.info.z < b.info.z; });
    ESP_LOGI(TAG, "%s: %u zoom (%d..%d), %u tile, %u bit/pixel", name,
             (unsigned)_zooms.size(), firstZoom(), lastZoom(),
             (unsigned)std::accumulate(_zooms.begin(), _zooms.end(), 0u,
                                       [](uint32_t acc, const ZoomRec& r) { return acc + r.info.present; }),
             (unsigned)_bits);
    _scratchCap = kMaxScratch;
    _scratch = (uint8_t*)psramAlloc(_scratchCap);
    if (!_scratch) {
        _scratchCap = 0;
        setError("PSRAM exhausted for the tile buffer");
        close();
        return false;
    }
    // The decompressor state lives on the heap on purpose: see the member comment.
    // Internal RAM first (it is touched word by word by ROM code), PSRAM as fallback.
    _inflate = malloc(sizeof(tinfl_decompressor));
    if (!_inflate) _inflate = psramAlloc(sizeof(tinfl_decompressor));
    if (!_inflate) {
        setError("memory for the decompressor");
        close();
        return false;
    }
    return true;
}

const MapPack::ZoomInfo* MapPack::findZoom(int z) const {
    for (const auto& rec : _zooms) {
        if (rec.info.z == z) return &rec.info;
    }
    return nullptr;
}

bool MapPack::loadIndex(const ZoomRec& rec) {
    releaseIndex();
    const size_t presenceBytes = ((size_t)rec.info.nx * rec.info.ny + 7) / 8;
    const size_t prefixBytes = ((size_t)rec.info.ny + 1) * 4;
    _presence = (uint8_t*)psramAlloc(presenceBytes);
    _prefix = (uint32_t*)psramAlloc(prefixBytes);
    if (!_presence || !_prefix) {
        releaseIndex();
        setError("PSRAM exhausted for the index");
        return false;
    }
    if (!_file.seek(rec.offPresence) || _file.read(_presence, presenceBytes) != (int)presenceBytes ||
        !_file.seek(rec.offPrefix) || _file.read((uint8_t*)_prefix, prefixBytes) != (int)prefixBytes) {
        releaseIndex();
        setError("zoom index unreadable");
        return false;
    }
    if (rec.info.present > 0) {
        const size_t indexBytes = (size_t)rec.info.present * kTileEntrySize;
        if (indexBytes <= kIndexPsramLimit) {
            _index = (uint8_t*)psramAlloc(indexBytes);
            if (_index) {
                if (!_file.seek(rec.offIndex) || _file.read(_index, indexBytes) != (int)indexBytes) {
                    releaseIndex();
                    setError("tile table unreadable");
                    return false;
                }
                _indexCount = rec.info.present;
            } else {
                ESP_LOGW(TAG, "index of %u KB not in PSRAM: reading on demand",
                         (unsigned)(indexBytes / 1024));
            }
        } else {
            ESP_LOGW(TAG, "index of %u KB over the limit: reading on demand",
                     (unsigned)(indexBytes / 1024));
        }
    }
    _sel = &rec;
    _selBits = rec.info.bits;
    return true;
}

bool MapPack::selectZoom(int z) {
    if (!_file) {
        setError("container not open");
        return false;
    }
    if (_sel && _sel->info.z == z) return true;
    for (const auto& rec : _zooms) {
        if (rec.info.z == z) return loadIndex(rec);
    }
    setError("zoom not present in the container");
    return false;
}

size_t MapPack::packedTileBytes() const {
    return (size_t)_tilePx * _tilePx * _selBits / 8;
}

bool MapPack::tileAt(int x, int y, uint32_t& offset, uint16_t& length, uint16_t& flags) const {
    offset = 0;
    length = 0;
    flags = 0;
    if (!_sel || !_presence || !_prefix) return false;
    if (x < (int)_sel->info.x0 || y < (int)_sel->info.y0) return false;
    const uint32_t lx = (uint32_t)x - _sel->info.x0;
    const uint32_t ly = (uint32_t)y - _sel->info.y0;
    if (lx >= _sel->info.nx || ly >= _sel->info.ny) return false;
    const uint32_t bit = ly * _sel->info.nx + lx;
    if (!(_presence[bit >> 3] & (0x80 >> (bit & 7)))) return false;

    int rank = (int)_prefix[ly] + popcountRange(_presence, ly * _sel->info.nx, bit);
    if (rank < 0 || (uint32_t)rank >= _sel->info.present) return false;
    uint8_t entry[kTileEntrySize];
    if (_index && _indexCount > 0) {
        memcpy(entry, _index + (size_t)rank * kTileEntrySize, kTileEntrySize);
    } else {
        // Big index: one small read for the single entry we need.
        if (!_file.seek(_sel->offIndex + (uint32_t)rank * kTileEntrySize) ||
            _file.read(entry, kTileEntrySize) != (int)kTileEntrySize) {
            return false;
        }
    }
    offset = rd32(entry);
    length = rd16(entry + 4);
    flags = rd16(entry + 6);
    return true;
}

bool MapPack::readTile(int x, int y, uint8_t* out, size_t outCap, size_t& outLen) {
    outLen = 0;
    uint32_t offset = 0;
    uint16_t length = 0, flags = 0;
    if (!tileAt(x, y, offset, length, flags)) return false;
    if ((flags & kTileFlagBlank) || length == 0) return false;   // blank: the caller fills it
    if (!out || outCap < packedTileBytes() || !_scratch || length > _scratchCap) return false;
    if (!_file.seek(offset) || _file.read(_scratch, length) != (int)length) {
        ESP_LOGW(TAG, "tile read %d/%d/%d failed", (int)(_sel ? _sel->info.z : 0), x, y);
        return false;
    }
    if (!_inflate) return false;
    auto* inflate = (tinfl_decompressor*)_inflate;
    tinfl_init(inflate);
    size_t inLen = length;
    size_t produced = outCap;
    // NON_WRAPPING: the output buffer holds the whole tile and doubles as the
    // 32 KB dictionary, so no extra window buffer is needed.
    const tinfl_status status = tinfl_decompress(
        inflate, _scratch, &inLen, out, out, &produced,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || produced != packedTileBytes()) {
        ESP_LOGW(TAG, "inflate tile %d/%d/%d: status %d, %u bytes (expected %u)",
                 (int)(_sel ? _sel->info.z : 0), x, y, (int)status, (unsigned)produced,
                 (unsigned)packedTileBytes());
        return false;
    }
    outLen = produced;
    return true;
}

namespace {
void tileToLatLon(double tx, double ty, int z, double& lat, double& lon) {
    const double n = pow(2.0, z);
    lon = tx / n * 360.0 - 180.0;
    const double latRad = atan(sinh(M_PI * (1.0 - 2.0 * ty / n)));
    lat = latRad * 57.29577951308232;
}
} // namespace

bool MapPack::selectedBounds(double& minLat, double& minLon, double& maxLat, double& maxLon) const {
    if (!_sel) return false;
    const int z = _sel->info.z;
    double lat1, lon1, lat2, lon2;
    tileToLatLon(_sel->info.x0, _sel->info.y0, z, lat1, lon1);
    tileToLatLon(_sel->info.x0 + _sel->info.nx, _sel->info.y0 + _sel->info.ny, z, lat2, lon2);
    minLat = std::min(lat1, lat2);
    maxLat = std::max(lat1, lat2);
    minLon = std::min(lon1, lon2);
    maxLon = std::max(lon1, lon2);
    return true;
}

bool MapPack::selectedCentre(int& tileX, int& tileY) const {
    if (!_sel) return false;
    tileX = (int)_sel->info.x0 + (int)(_sel->info.nx / 2);
    tileY = (int)_sel->info.y0 + (int)(_sel->info.ny / 2);
    // A sparse collection can leave the middle uncovered: fall back to the first
    // covered tile so the "find my maps" recentring never lands on an empty area.
    uint32_t offset; uint16_t length, flags;
    for (uint32_t yy = 0; yy < _sel->info.ny; ++yy) {
        for (uint32_t xx = 0; xx < _sel->info.nx; ++xx) {
            if (tileAt((int)(_sel->info.x0 + xx), (int)(_sel->info.y0 + yy), offset, length, flags)) {
                tileX = (int)(_sel->info.x0 + xx);
                tileY = (int)(_sel->info.y0 + yy);
                return true;
            }
        }
    }
    return true;
}

size_t MapPack::listRegions(std::vector<std::string>& out) {
    out.clear();
    const char* roots[2] = {"/maps", "/sdcard/maps"};
    for (const char* root : roots) {
        File dir = SD_MMC.open(root);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            continue;
        }
        while (File entry = dir.openNextFile()) {
            if (!entry.isDirectory()) {
                const char* name = entry.name();
                const char* base = name ? strrchr(name, '/') : nullptr;
                base = base ? base + 1 : name;
                const size_t len = base ? strlen(base) : 0;
                if (len > 5 && strcasecmp(base + len - 5, ".mmap") == 0) {
                    char full[96];
                    snprintf(full, sizeof(full), "%s/%s", root, base);
                    out.push_back(full);
                }
            }
            entry.close();
        }
        dir.close();
        if (!out.empty()) break;
    }
    std::sort(out.begin(), out.end());
    return out.size();
}

} // namespace MonoMesh
