#pragma once

// Vector map container (.mmap v2) reader for the firmware (plan Fase 3).
//
// MapPack stays the reader for the raster containers (v1 regions and the
// global basemap); this class serves the vector bands through libmapcore's
// mm_vmap reader, which is the same code the host tools fuzz and golden test.
//
// Chunk cache (budget D-0.6, measured in scratch/spike_piano_b.md):
//  * band L ("overview", z7-11) is pinned whole: its decompressed payloads
//    (~1.8 MB on the reference container) make every z7-11 pan instant after
//    the first load, where the 512 KB ring alone would thrash;
//  * bands M/H live in a 16 slot LRU ring of decompressed payloads;
//  * reads are whole-chunk SD reads (multi-KB sequential, never scattered).
#include <Arduino.h>
#include <FS.h>

extern "C" {
#include "mm_vmap.h"
#include "mm_render.h"
}

namespace MonoMesh {

class MapVector {
public:
    // Budget D-0.6 (revised after measurement): 1 MB of decompressed
    // payloads for the M/H ring.  The ring is limited by BYTES, not entries:
    // a z12 Milan screen needs ~750 KB across ~30 chunks, and both a slot
    // counted ring (95 SD reads per frame) and the original 512 KB cap thrash
    // on it.  8 MB PSRAM: canvas 280 KB + this ring 1 MB + overview pin ~2 MB.
    static constexpr size_t kRingBytes = 1024 * 1024;
    static constexpr int kRingMaxEntries = 256;
    static constexpr size_t kChunkMaxBytes = 64 * 1024;   // M/H chunk cap + PSRAM scratch
    static constexpr size_t kLBandMaxBytes = 3 * 1024 * 1024;  // pinned overview band

    ~MapVector() { close(); }

    bool open(const char* path);
    void close();
    bool isOpen() const { return _open; }

    const char* name() const { return _map.name; }
    int zmin() const;
    int zmax() const;
    const mm_vmap* map() const { return &_map; }
    mm_chunks* chunks() { return &_chunks; }

    // Diagnostics for the frame log (plan Fase 3.5).
    uint32_t chunkReads() const { return _reads; }
    uint32_t chunkHits() const { return _hits; }
    uint32_t windowFills() const { return _winFills; }
    size_t payloadBytes() const { return _payloadBytes; }
    size_t pinnedBytes() const { return _lLen; }
    size_t ringBytes() const { return _ringBytes; }
    size_t preloadBytes() const { return _preBytes; }
    void resetStats() { _reads = 0; _hits = 0; _payloadBytes = 0; _winFills = 0; }

private:
    struct Slot {
        uint8_t* data = nullptr;
        size_t len = 0;
        uint32_t stamp = 0;
        uint32_t cellX = 0, cellY = 0;
        uint8_t band = 0;
        mm_chunk_dir dir = {};   // record directory, built on first use
        uint32_t dirBytes = 0;   // counted against the ring directory budget
        bool dirNone = false;    // over the cap/OOM: never retry this chunk
    };

    // Record directories: 10 B per record (compact two-array layout, see
    // mm_vmap.h).  Real containers hold cells with ~90k tiny records: at the
    // old 20 B/record a single overview cell cost megabytes.  The ring (M/H)
    // gets a per-chunk cap and a total budget; the pinned overview band gets
    // its own budget with LRU eviction so the hot cells (Milan) fit.
    static constexpr uint32_t kDirMaxRecords = 4096;         // 40 KB per M/H chunk
    static constexpr size_t kRingDirBudget = 512 * 1024;
    static constexpr uint32_t kLDirMaxRecords = 200000;      // ~2 MB single cell cap
    static constexpr size_t kLDirBudget = 1536 * 1024;

    static int64_t ioRead(void* ctx, uint64_t off, void* buf, size_t len);
    static int chunkTrampoline(void* user, int band, uint32_t cx, uint32_t cy,
                               const uint8_t** data, size_t* len, uint16_t* flags);
    static int dirTrampoline(void* user, int band, uint32_t cx, uint32_t cy,
                             const mm_chunk_dir** dir);
    int getChunk(int band, uint32_t cx, uint32_t cy, const uint8_t** data,
                 size_t* len, uint16_t* flags);
    int getDir(int band, uint32_t cx, uint32_t cy, const mm_chunk_dir** dir);
    void ringEvict(size_t need);
    bool loadBandL();

    // Bulk preload of the immutable index + label sections (plan D6: SD only
    // multi-KB sequential).  These are the sections the label pass used to walk
    // with hundreds of scattered reads per frame (the z11 "10 seconds").
    bool preloadIndexData();
    bool preloadAdd(uint64_t off, size_t len, uint8_t** dst, size_t* dstLen);
    const uint8_t* preloadLookup(uint64_t off, size_t len) const;
    void preloadFree();

    File _file;
    mm_vmap _map = {};
    mm_vmap_io _io = {};
    mm_chunks _chunks = {};
    bool _open = false;

    Slot _ring[kRingMaxEntries];
    int _ringN = 0;
    size_t _ringBytes = 0;
    uint32_t _clock = 0;

    size_t _dirBytes = 0;          // ring directories total (budget kRingDirBudget)
    uint8_t* _scratch = nullptr;   // PSRAM inflate scratch for the ring path

    // Read-ahead window: the index walks (presence popcount, row prefix, POI
    // refs) are hundreds of tiny reads, and D6 forbids scattered sub-512B SD
    // access.  Everything under kWinCap bytes is served from this window.
    // After preloadIndexData() the label sections never reach this path at all.
    static constexpr size_t kWinCap = 8 * 1024;
    uint8_t* _win = nullptr;
    uint64_t _winOff = 0;
    size_t _winLen = 0;

    // Preloaded immutable sections: band presence/prefix, POIGRID arrays,
    // SEC_POI and SEC_STRINGS.  ioRead() serves them straight from PSRAM.
    static constexpr int kPreloadMax = 16;
    static constexpr size_t kPreloadCap = 2 * 1024 * 1024;
    struct Preload {
        uint64_t off = 0;
        size_t len = 0;
        uint8_t* data = nullptr;
    };
    Preload _pre[kPreloadMax];
    int _preN = 0;
    size_t _preBytes = 0;
    bool _preFailed = false;

    // pinned overview band (band 0): chunk payloads kept decompressed
    uint8_t* _lData = nullptr;
    size_t _lLen = 0, _lCap = 0;
    struct LChunk {
        uint32_t offset, len;
        uint16_t flags;
    };
    LChunk* _lChunks = nullptr;   // one per band-0 grid cell
    mm_chunk_dir* _lDirs = nullptr; // overview band directories (budget + LRU)
    uint8_t* _lDirNone = nullptr;   // cell decided against a directory: no retry
    uint32_t* _lDirBytesArr = nullptr;
    uint32_t* _lDirStamp = nullptr;
    uint32_t _lDirClock = 0;
    size_t _lDirBytes = 0;
    uint32_t _lCells = 0;

    uint32_t _reads = 0, _hits = 0, _winFills = 0;
    size_t _payloadBytes = 0;
    bool _lFailed = false;   // arena growth failed: stop retrying every frame
};

}  // namespace MonoMesh
