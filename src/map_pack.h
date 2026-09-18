#pragma once

#include <Arduino.h>
#include <FS.h>
#include <string>
#include <vector>

namespace MonoMesh {

// Read-only accessor for the MonoMesh packed map container (.mmap, format v1, see
// tools/mono_map.py for the writer and the field by field description).
//
// Why a container instead of /tiles/z/x/y.png:
//  * one SD file instead of tens of thousands, so copying a region is a single
//    write and FAT32 does not pay a cluster per tile;
//  * a tile lookup is pure arithmetic on a small index kept in PSRAM: no
//    directory walk, no per-tile open/close;
//  * the payload is the packed monochrome bitmap, so the map view never runs a
//    PNG decoder: one SD read and one inflate per tile (or a skip for the
//    thousands of empty tiles, which carry no payload at all).
class MapPack {
public:
    struct ZoomInfo {
        uint8_t z = 0;
        uint8_t bits = 1;
        uint32_t x0 = 0, nx = 0, y0 = 0, ny = 0;
        uint32_t present = 0;
        uint32_t payloadBytes = 0;
    };

private:
    // ZoomInfo plus the absolute file offsets of that level's index blocks.
    struct ZoomRec {
        ZoomInfo info;
        uint32_t offPresence = 0;
        uint32_t offPrefix = 0;
        uint32_t offIndex = 0;
        uint32_t offPayload = 0;
    };

public:
    static constexpr size_t kMaxScratch = 32 * 1024;   // largest compressed tile accepted
    static constexpr size_t kIndexPsramLimit = 1536 * 1024;

    ~MapPack();

    bool open(const char* path);
    void close();
    bool isOpen() const { return (bool)_file; }

    const std::string& name() const { return _name; }
    const std::string& attribution() const { return _attribution; }
    const std::string& path() const { return _path; }
    bool dark() const { return _dark; }
    uint16_t tilePx() const { return _tilePx; }
    size_t zoomCount() const { return _zooms.size(); }
    const ZoomInfo* zoomAt(size_t i) const { return i < _zooms.size() ? &_zooms[i].info : nullptr; }

    const ZoomInfo* findZoom(int z) const;
    bool hasZoom(int z) const { return findZoom(z) != nullptr; }
    int firstZoom() const { return _zooms.empty() ? 0 : (int)_zooms.front().info.z; }
    int lastZoom() const { return _zooms.empty() ? 0 : (int)_zooms.back().info.z; }

    // Keeps the index of one zoom level in RAM: presence bitmap, per-row prefix
    // and the tile entry array. Idempotent, cheap when the zoom is already set.
    bool selectZoom(int z);
    int selectedZoom() const { return _sel ? (int)_sel->info.z : -1; }
    uint8_t selectedBits() const { return _selBits; }
    size_t packedTileBytes() const;
    // Tiles that carry no payload are filled with black in a dark style.
    bool blankIsBlack() const { return _dark; }

    // Tile access for the currently selected zoom.
    bool tileAt(int x, int y, uint32_t& offset, uint16_t& length, uint16_t& flags) const;
    // Reads and inflates one tile into `out` (packed bits, packedTileBytes() long).
    // Returns false for a blank tile or on any error, so the caller paints the
    // background instead of garbage.
    bool readTile(int x, int y, uint8_t* out, size_t outCap, size_t& outLen);

    // Geographic bounds of the selected zoom's tile grid ("find my maps" without
    // touching the filesystem) and the centre of the covered area in tile units.
    bool selectedBounds(double& minLat, double& minLon, double& maxLat, double& maxLon) const;
    bool selectedCentre(int& tileX, int& tileY) const;

    static size_t listRegions(std::vector<std::string>& out);
    const char* lastError() const { return _error.empty() ? "" : _error.c_str(); }

private:
    bool loadIndex(const ZoomRec& rec);
    void releaseIndex();
    void setError(const char* msg);

    mutable File _file;   // tileAt() is const but has to read entries from the SD
    std::string _path, _name, _attribution, _error;
    uint16_t _tilePx = 256;
    uint8_t _bits = 1;
    bool _dark = false;
    std::vector<ZoomRec> _zooms;

    const ZoomRec* _sel = nullptr;
    uint8_t _selBits = 1;
    uint8_t* _presence = nullptr;
    uint32_t* _prefix = nullptr;
    uint8_t* _index = nullptr;    // nullptr => entries are read from the SD on demand
    size_t _indexCount = 0;
    uint8_t* _scratch = nullptr;
    size_t _scratchCap = 0;
    // tinfl_decompressor*, heap allocated: the ROM helper keeps this ~6 KB state
    // (plus the window bookkeeping) on the caller's stack, and the Arduino loop
    // task only has 8 KB - it overflowed the moment the first tile was inflated.
    void* _inflate = nullptr;
};

} // namespace MonoMesh
