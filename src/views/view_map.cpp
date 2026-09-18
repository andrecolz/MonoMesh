#include "view_map.h"
#include "../mesh_service.h"
#include "../ui_engine.h"
#include "../storage_manager.h"
#include "../bsp_papermono.h"
#include "../epd_driver.h"
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>

namespace MonoMesh {

namespace {
constexpr double DEG2RAD = 0.017453292519943295;
constexpr double RAD2DEG = 57.29577951308232;
constexpr double TILE_SIZE = 256.0;
// A legacy PNG is a few KB; a packed tile is at most 16 KB (2 bit/pixel).
constexpr size_t TILE_MAX_BYTES = 64 * 1024;
constexpr size_t TILE_HEAP_FALLBACK_MAX = 32 * 1024;

// File::name() can return the full path depending on the core: keep only the last component
const char* baseName(const char* path) {
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// True when the directory exists and holds at least one regular file
bool dirHasFile(const char* dirPath) {
    File d = SD_MMC.open(dirPath);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return false;
    }
    bool found = false;
    while (File e = d.openNextFile()) {
        if (!e.isDirectory()) {
            found = true;
            e.close();
            break;
        }
        e.close();
    }
    d.close();
    return found;
}

// Distance in km on a small scale (equirectangular: plenty for the info card)
double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double dLat = (lat2 - lat1) * 111.320;
    const double dLon = (lon2 - lon1) * 111.320 * cos(lat1 * DEG2RAD);
    return sqrt(dLat * dLat + dLon * dLon);
}

void tileToLatLon(double tx, double ty, int z, double& lat, double& lon) {
    const double n = pow(2.0, z);
    lon = tx / n * 360.0 - 180.0;
    const double latRad = atan(sinh(M_PI * (1.0 - 2.0 * ty / n)));
    lat = latRad * RAD2DEG;
}

// Packed-bit expansion tables. The four grey levels are the values the SSD1677
// maps to exactly one level each (no extra dithering on the device), so a 2 bit
// tile reproduces the anti-aliased labels of the old 8-bit PNGs.
const uint8_t* expandTable1() {
    static uint8_t table[256][8];
    static bool ready = false;
    if (!ready) {
        for (int v = 0; v < 256; ++v) {
            for (int k = 0; k < 8; ++k) table[v][k] = (v & (0x80 >> k)) ? 255 : 0;
        }
        ready = true;
    }
    return &table[0][0];
}

const uint8_t* expandTable2() {
    static const uint8_t levels[4] = {0, 96, 160, 255};
    static uint8_t table[256][4];
    static bool ready = false;
    if (!ready) {
        for (int v = 0; v < 256; ++v) {
            for (int k = 0; k < 4; ++k) table[v][k] = levels[(v >> (6 - 2 * k)) & 3];
        }
        ready = true;
    }
    return &table[0][0];
}
} // namespace

ViewMap::ViewMap() {
    StorageManager::getInstance().setSdReleaseHook(&ViewMap::sdReleaseHook, this);
}

uint32_t ViewMap::s_focusNodeNum = 0;
int ViewMap::s_focusZoom = 0;

void ViewMap::focusNodeOnMap(uint32_t nodeNum, int zoom) {
    s_focusNodeNum = nodeNum;
    s_focusZoom = zoom;
    UIEngine::getInstance().setView(2); // the request is applied in onEnter()
}

void ViewMap::sdReleaseHook(void* self) {
    if (self) static_cast<ViewMap*>(self)->releaseSdHandles();
}

void ViewMap::releaseSdHandles() {
    // Called before SD_MMC.end(): drop the container handle and everything read from
    // that card. The next update() notices the card state change and reopens it.
    _pack.close();
    _usePack = false;
    invalidateTileCache();
    // Force the next update() through the "card changed" path, which re-scans and
    // reopens the container once the card is mounted again after the wake.
    _sdWasMounted = false;
}

ViewMap::~ViewMap() {
    StorageManager::getInstance().clearSdReleaseHook(this);
    releaseCanvas();
    _pack.close();
    for (auto& slot : _tileCache) {
        if (slot.data) free(slot.data);
        slot.data = nullptr;
        slot.len = 0;
    }
}

// ---------------------------------------------------------------- lifecycle

bool ViewMap::ensureCanvas() {
    if (_canvasReady) return true;
    _canvas.setPsram(true);
    // Greyscale, not RGB332: the tile blitter writes 8-bit grey values straight
    // into the buffer and the panel expects exactly those values.
    _canvas.setColorDepth(lgfx::grayscale_8bit);
    if (!_canvas.createSprite(480, CV_H)) {
        ESP_LOGE("ViewMap", "map canvas not allocated in PSRAM (%d KB)", (480 * CV_H) / 1024);
        return false;
    }
    _canvas.fillSprite(TFT_WHITE);
    _canvas.setTextWrap(false);
    _canvasReady = true;
    ESP_LOGI("ViewMap", "map canvas %dx%d in PSRAM (%d KB)", 480, CV_H, (480 * CV_H) / 1024);
    return true;
}

void ViewMap::releaseCanvas() {
    if (_canvasReady) {
        _canvas.deleteSprite();
        _canvasReady = false;
    }
}

void ViewMap::toast(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_toast, sizeof(_toast), fmt, ap);
    va_end(ap);
    _toastUntilMs = millis() + 2500;
}

void ViewMap::onEnter() {
    ensureCanvas();

    const bool sd = StorageManager::getInstance().isSdMounted();
    if (!_viewInitialised || sd != _sdWasMounted) {
        _sdWasMounted = sd;
        scanRegions();
        if (!_regions.empty()) {
            if (_regionIndex >= _regions.size()) _regionIndex = 0;
            if (!openRegion(_regionIndex, false)) scanZooms();   // broken container: PNG fallback
        } else {
            _pack.close();
            _usePack = false;
            scanZooms();
        }
        invalidateTileCache(); // the card changed: cached tiles may belong to another card
    } else if (_zooms.empty()) {
        scanRegions();
        if (!_regions.empty() && !_usePack) openRegion(_regionIndex, false);
        if (!_usePack) scanZooms();
    }

    if (!_viewInitialised) {
        _viewInitialised = true;
        restoreSavedView();             // no-op (false) on the very first boot
        ensureZoomAvailable();          // snap the zoom to a level that really exists
        // "Find my maps": never sit on an empty area if the card holds a map. Our own position wins,
        // then the area the map actually covers.
        if (!pickZoomWithTile()) {
            if (!centerOnNode() || !pickZoomWithTile()) {
                if (_usePack) {
                    if (centerOnPack()) pickZoomWithTile();
                } else if (centerOnAvailableTiles()) {
                    pickZoomWithTile();
                }
            }
        }
        saveView(true);
    } else {
        ensureZoomAvailable();
        // The card may have been swapped, or the saved view may point at an uncovered area: re-centre
        // on the area we actually hold instead of showing white until the user taps the header button.
        if (!_zooms.empty() && !pickZoomWithTile()) {
            const bool recentred = _usePack ? centerOnPack() : centerOnAvailableTiles();
            if (recentred) {
                pickZoomWithTile();
                saveView(true); // keep the new centre, so the next entry does not have to re-scan
            }
        }
    }

    _dirtyFast = false;
    _needsSettle = false; // the full redraw that follows onEnter() already paints in grayscale
    _lastMoveMs = 0;
    _pendingDx = _pendingDy = 0;

    applyPendingFocus();
}

// "VIEW ON MAP" from the node list: centre on that node, at the requested zoom when the card really
// has that level here (otherwise the closest one that does).
void ViewMap::applyPendingFocus() {
    if (s_focusNodeNum == 0) return;
    const MeshNode* n = MeshService::getInstance().findNode(s_focusNodeNum);
    if (n && n->hasPosition && (n->lat != 0.0 || n->lon != 0.0)) {
        _centerLat = n->lat;
        _centerLon = n->lon;
        _selectedNodeNum = n->nodeNum;
        if (s_focusZoom > 0) {
            _zoomLevel = s_focusZoom;
            if (_usePack) _pack.selectZoom(_zoomLevel);
            ensureZoomAvailable();
            pickZoomWithTile();
        }
        invalidateTileCache();
        saveView(true);
        requestFastRedraw(ZOOM_SETTLE_MS); // also covers the case of a focus request while already here
    } else {
        ESP_LOGW("ViewMap", "VIEW ON MAP: !%08x without coordinates", (unsigned)s_focusNodeNum);
    }
    s_focusNodeNum = 0;
    s_focusZoom = 0;
}

void ViewMap::update() {
    applyPendingFocus();
    // SD hot-plug: rescan the maps and drop the cached tiles when the state changes
    const bool sd = StorageManager::getInstance().isSdMounted();
    if (sd != _sdWasMounted) {
        _sdWasMounted = sd;
        scanRegions();
        if (!_regions.empty()) {
            if (_regionIndex >= _regions.size()) _regionIndex = 0;
            openRegion(_regionIndex, false);
        } else {
            _pack.close();
            _usePack = false;
            scanZooms();
        }
        invalidateTileCache();
        ensureZoomAvailable();
        _dirtyFast = true;
    }

    if (_toastUntilMs && (int32_t)(millis() - _toastUntilMs) >= 0) {
        _toastUntilMs = 0;
        _toast[0] = '\0';
        _dirtyFast = true;
    }

    if (_dirtyFast) {
        renderFull(false); // 1-bit, no flash: the map follows the finger
        _dirtyFast = false;
    }

    // One grayscale pass after the user stopped: 4 sharp levels again. The delay is 1 s after a pan
    // and a little longer after a button zoom, so stepping 15 -> 12 does not trigger a refresh (and
    // its flash) between two clicks.
    if (_needsSettle && (uint32_t)(millis() - _lastMoveMs) >= _settleMs) {
        renderFull(true);
        _needsSettle = false;
        saveView(false);
    }
}

void ViewMap::requestFastRedraw(uint32_t settleMs) {
    _dirtyFast = true;
    _needsSettle = true;
    _lastMoveMs = millis();
    _settleMs = settleMs ? settleMs : SETTLE_MS;
}

// ---------------------------------------------------------------- canvas

void ViewMap::publishRect(int canvasY0, int canvasY1, bool grayscale) {
    if (!_canvasReady) return;
    canvasY0 = std::max(0, canvasY0);
    canvasY1 = std::min(CV_H, canvasY1);
    if (canvasY0 >= canvasY1) return;
    const uint8_t* buf = (const uint8_t*)_canvas.getBuffer() + (size_t)canvasY0 * 480;
    M5.Display.pushImage(0, MAP_TOP + canvasY0, 480, canvasY1 - canvasY0,
                         (const lgfx::grayscale_t*)buf);
    if (grayscale) {
        EPDDriver::getInstance().flushRectGrayscale(0, MAP_TOP + canvasY0, 480, canvasY1 - canvasY0);
    } else {
        // countPartial=false: dragging must never trigger an anti-ghosting full clear mid-gesture
        EPDDriver::getInstance().flushRect(0, MAP_TOP + canvasY0, 480, canvasY1 - canvasY0, false);
    }
}

void ViewMap::scrollCanvas(int dx, int dy) {
    if (!_canvasReady || (dx == 0 && dy == 0)) return;
    uint8_t* buf = (uint8_t*)_canvas.getBuffer();
    if (dy > 0) {
        memmove(buf + (size_t)dy * 480, buf, (size_t)(CV_H - dy) * 480);
        memset(buf, 0xFF, (size_t)dy * 480);
    } else if (dy < 0) {
        const int ady = -dy;
        memmove(buf, buf + (size_t)ady * 480, (size_t)(CV_H - ady) * 480);
        memset(buf + (size_t)(CV_H - ady) * 480, 0xFF, (size_t)ady * 480);
    }
    if (dx != 0) {
        for (int y = 0; y < CV_H; ++y) {
            uint8_t* row = buf + (size_t)y * 480;
            if (dx > 0) {
                memmove(row + dx, row, (size_t)(480 - dx));
                memset(row, 0xFF, (size_t)dx);
            } else {
                const int adx = -dx;
                memmove(row, row + adx, (size_t)(480 - adx));
                memset(row + (480 - adx), 0xFF, (size_t)adx);
            }
        }
    }
}

void ViewMap::renderFull(bool grayscale) {
    if (!ensureCanvas()) return;
    drawHeader(M5.Display);
    composeScene(0, 0, 480, CV_H);
    M5.Display.pushImage(0, MAP_TOP, 480, CV_H, (const lgfx::grayscale_t*)_canvas.getBuffer());
    drawInfoCard(M5.Display); // below the map area, outside the canvas
    // The header carries the zoom level, so it is repainted together with the map and both go out in
    // the same panel update: 44..729, the status bar above stays untouched.
    if (grayscale) {
        EPDDriver::getInstance().flushRectGrayscale(0, MAP_VIEW_Y, 480, MAP_BOTTOM - MAP_VIEW_Y + 1);
    } else {
        EPDDriver::getInstance().flushRect(0, MAP_VIEW_Y, 480, MAP_BOTTOM - MAP_VIEW_Y + 1, false);
    }
    _needsSettle = false;
}

// ---------------------------------------------------------------- region

void ViewMap::scanRegions() {
    _regions.clear();
    if (!StorageManager::getInstance().isSdMounted()) return;
    MapPack::listRegions(_regions);
    if (!_regions.empty()) {
        ESP_LOGI("ViewMap", "%u .mmap containers in /maps", (unsigned)_regions.size());
    }
}

bool ViewMap::openRegion(size_t index, bool announce) {
    if (index >= _regions.size()) return false;
    _regionIndex = index;
    if (_pack.open(_regions[index].c_str()) && _pack.tilePx() == TILE_PX) {
        _usePack = true;
        _zooms.clear();
        for (size_t i = 0; i < _pack.zoomCount(); ++i) {
            if (const MapPack::ZoomInfo* zi = _pack.zoomAt(i)) _zooms.push_back(zi->z);
        }
        std::sort(_zooms.begin(), _zooms.end());
        invalidateTileCache();
        if (announce) {
            toast("Map: %s (Z %d-%d)", _pack.name().c_str(), _pack.firstZoom(), _pack.lastZoom());
        }
        ESP_LOGI("ViewMap", "active container: %s", _regions[index].c_str());
        return true;
    }
    if (_pack.isOpen()) {
        // Tile size other than the one this view draws: refuse instead of
        // scrambling the map (the generator always writes 256 unless asked).
        if (announce) toast("Tile %dpx not supported: %s", (int)_pack.tilePx(), baseName(_regions[index].c_str()));
        _pack.close();
    } else if (announce) {
        toast("Unreadable map: %s", baseName(_regions[index].c_str()));
    }
    _usePack = false;
    _zooms.clear();
    return false;
}

bool ViewMap::centerOnPack() {
    if (!_usePack) return false;
    const int z = _zooms.empty() ? _pack.firstZoom() : _zooms.front();
    if (!_pack.selectZoom(z)) return false;
    int tx = 0, ty = 0;
    if (!_pack.selectedCentre(tx, ty)) return false;
    tileToLatLon(tx + 0.5, ty + 0.5, z, _centerLat, _centerLon);
    _zoomLevel = z;
    _selectedNodeNum = 0;
    ESP_LOGI("ViewMap", "centred on container: %.5f, %.5f (Z%d)", _centerLat, _centerLon, z);
    return true;
}

// ---------------------------------------------------------------- tiles

std::string ViewMap::tilePath(int z, int x, int y, bool legacyRoot) const {
    char path[80];
    // "/tiles/..." is the real path: SD_MMC.begin("/sdcard") makes the FS API prepend the mount
    // point, so passing "/sdcard/tiles/..." would resolve to /sdcard/sdcard/tiles/... and never
    // exist (every tile lookup paid a failing stat before finding the working path).
    snprintf(path, sizeof(path), legacyRoot ? "/sdcard/tiles/%d/%d/%d.png" : "/tiles/%d/%d/%d.png", z, x, y);
    return std::string(path);
}

int ViewMap::acquireTileSlot(int z, int x, int y) {
    for (int i = 0; i < TILE_SLOTS; ++i) {
        TileSlot& s = _tileCache[i];
        if (s.state == SLOT_EMPTY || s.z != z || s.x != x || s.y != y) continue;
        s.lastUse = ++_tileUseCounter;
        if (s.state != SLOT_MISSING) return (int)i;
        // Cached miss: keep it while it is fresh (a tile that is really not on the card must not be
        // probed on every frame), but re-check it after a while so a transient SD error - the card
        // was just mounted or busy - does not leave that tile blank for the whole session.
        if ((uint32_t)(millis() - s.missMs) < MISS_RETRY_MS) return -1;
        if (loadSlot(i, z, x, y)) return (int)i;
        s.missMs = millis();
        return -1;
    }
    int victim = 0;
    for (int i = 1; i < TILE_SLOTS; ++i) {
        if (_tileCache[i].lastUse < _tileCache[victim].lastUse) victim = i;
    }
    TileSlot& slot = _tileCache[victim];
    if (slot.data) {
        free(slot.data);
        slot.data = nullptr;
        slot.len = 0;
    }
    slot.z = z;
    slot.x = x;
    slot.y = y;
    slot.lastUse = ++_tileUseCounter;
    slot.state = SLOT_EMPTY;
    if (!loadSlot(victim, z, x, y)) {
        slot.state = SLOT_MISSING; // negative cache: do not hammer the SD for a tile that is not there
        slot.missMs = millis();
        return -1;
    }
    return victim;
}

bool ViewMap::loadSlot(int slot, int z, int x, int y) {
    if (!StorageManager::getInstance().isSdMounted()) return false;
    TileSlot& s = _tileCache[slot];
    if (s.data) {
        free(s.data);
        s.data = nullptr;
        s.len = 0;
    }

    if (_usePack) {
        if (_pack.selectedZoom() != z && !_pack.selectZoom(z)) return false;
        const size_t bytes = _pack.packedTileBytes();
        uint32_t offset = 0;
        uint16_t length = 0, flags = 0;
        if (!_pack.tileAt(x, y, offset, length, flags)) return false;   // no tile here
        if ((flags & 1) || length == 0) {                                // empty tile: no payload
            s.state = _pack.blankIsBlack() ? SLOT_BLANK_BLACK : SLOT_BLANK_WHITE;
            return true;
        }
        uint8_t* buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (uint8_t*)malloc(bytes);
        if (!buf) return false;
        size_t got = 0;
        if (!_pack.readTile(x, y, buf, bytes, got)) {
            free(buf);
            return false;
        }
        s.data = buf;
        s.len = got;
        s.state = SLOT_PACKED;
        ++_statTileReads;
        return true;
    }

    // ---- legacy: /tiles/z/x/y.png ----
    std::string path = tilePath(z, x, y, false);
    if (!SD_MMC.exists(path.c_str())) {
        path = tilePath(z, x, y, true);
        if (!SD_MMC.exists(path.c_str())) return false;
    }
    File f = SD_MMC.open(path.c_str(), FILE_READ);
    if (!f) return false;
    const size_t size = f.size();
    if (size == 0 || size > TILE_MAX_BYTES) {
        f.close();
        return false;
    }
    // Read the whole image first: decoding straight from the stream let a slow/failed SD read
    // produce half a tile on screen, which is what left white bands in the middle of the map.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf && size <= TILE_HEAP_FALLBACK_MAX) buf = (uint8_t*)malloc(size);
    if (!buf) {
        f.close();
        return false;
    }
    size_t got = 0;
    while (got < size) {
        const int n = f.read(buf + got, size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    f.close();
    if (got != size) {
        free(buf);
        return false;
    }
    s.data = buf;
    s.len = size;
    s.state = SLOT_PNG;
    ++_statTileReads;
    return true;
}

void ViewMap::blitSlot(int slot, int canvasX, int canvasY) {
    if (!_canvasReady) return;
    const TileSlot& s = _tileCache[slot];
    switch (s.state) {
        case SLOT_PACKED: {
            const int bits = _pack.selectedBits();
            const int srcW = _pack.tilePx();
            const int srcRowBytes = srcW * bits / 8;
            const int dx0 = std::max(0, canvasX), dx1 = std::min(480, canvasX + srcW);
            const int dy0 = std::max(0, canvasY), dy1 = std::min(CV_H, canvasY + srcW);
            if (dx0 >= dx1 || dy0 >= dy1) return;
            uint8_t* buf = (uint8_t*)_canvas.getBuffer();
            if (bits == 1) {
                const uint8_t* lut = expandTable1();
                for (int y = dy0; y < dy1; ++y) {
                    const uint8_t* src = s.data + (size_t)(y - canvasY) * srcRowBytes;
                    uint8_t* dst = buf + (size_t)y * 480 + dx0;
                    int p = dx0 - canvasX;
                    const int n = dx1 - dx0;
                    int i = 0;
                    while (i < n && (p & 7)) { dst[i++] = lut[(src[p >> 3] << 3) + (p & 7)]; ++p; }
                    while (i + 8 <= n) { memcpy(dst + i, lut + ((size_t)src[p >> 3] << 3), 8); i += 8; p += 8; }
                    while (i < n) { dst[i++] = lut[(src[p >> 3] << 3) + (p & 7)]; ++p; }
                }
            } else {
                const uint8_t* lut = expandTable2();
                for (int y = dy0; y < dy1; ++y) {
                    const uint8_t* src = s.data + (size_t)(y - canvasY) * srcRowBytes;
                    uint8_t* dst = buf + (size_t)y * 480 + dx0;
                    int p = dx0 - canvasX;
                    const int n = dx1 - dx0;
                    int i = 0;
                    while (i < n && (p & 3)) { dst[i++] = lut[((size_t)src[p >> 2] << 2) + (p & 3)]; ++p; }
                    while (i + 4 <= n) { memcpy(dst + i, lut + ((size_t)src[p >> 2] << 2), 4); i += 4; p += 4; }
                    while (i < n) { dst[i++] = lut[((size_t)src[p >> 2] << 2) + (p & 3)]; ++p; }
                }
            }
            break;
        }
        case SLOT_PNG:
            if (!_canvas.drawPng(s.data, (uint32_t)s.len, canvasX, canvasY)) {
                _canvas.fillRect(canvasX, canvasY, TILE_PX, TILE_PX, TFT_WHITE);
            }
            break;
        case SLOT_BLANK_WHITE:
            _canvas.fillRect(canvasX, canvasY, TILE_PX, TILE_PX, TFT_WHITE);
            break;
        case SLOT_BLANK_BLACK:
            _canvas.fillRect(canvasX, canvasY, TILE_PX, TILE_PX, TFT_BLACK);
            break;
        default:
            break;
    }
}

void ViewMap::invalidateTileCache() {
    for (auto& slot : _tileCache) {
        if (slot.data) free(slot.data);
        slot.data = nullptr;
        slot.len = 0;
        slot.state = SLOT_EMPTY;
        slot.z = -1;
        slot.lastUse = 0;
    }
    _tileUseCounter = 0;
}

void ViewMap::scanZooms() {
    if (_usePack) return; // the container is the source of truth for the zoom levels
    _zooms.clear();
    if (!StorageManager::getInstance().isSdMounted()) return;

    const char* roots[2] = {"/tiles", "/sdcard/tiles"};
    for (const char* root : roots) {
        File dir = SD_MMC.open(root);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            continue;
        }
        while (File zEntry = dir.openNextFile()) {
            if (zEntry.isDirectory()) {
                int z = 0;
                if (parseInt(baseName(zEntry.name()), z) && z >= 1 && z <= 19) {
                    // Only count levels that really contain tiles (<z>/<x>/<y>.png): a stale empty
                    // directory must not become a zoom step.
                    char zPath[64];
                    snprintf(zPath, sizeof(zPath), "%s/%d", root, z);
                    File zd = SD_MMC.open(zPath);
                    bool ok = false;
                    if (zd && zd.isDirectory()) {
                        int inspected = 0;
                        while (File xe = zd.openNextFile()) {
                            if (xe.isDirectory() && ++inspected <= 4) {
                                char xPath[80];
                                snprintf(xPath, sizeof(xPath), "%s/%s", zPath, baseName(xe.name()));
                                if (dirHasFile(xPath)) ok = true;
                            }
                            xe.close();
                            if (ok || inspected > 4) break;
                        }
                    }
                    if (zd) zd.close();
                    if (ok) _zooms.push_back(z);
                }
            }
            zEntry.close();
        }
        dir.close();
        if (!_zooms.empty()) break; // prefer /tiles (mount point is /sdcard), fall back to the legacy root
    }

    std::sort(_zooms.begin(), _zooms.end());
    _zooms.erase(std::unique(_zooms.begin(), _zooms.end()), _zooms.end());
    if (!_zooms.empty()) {
        ESP_LOGI("ViewMap", "Tile zoom levels on SD: %d..%d (%u levels)", _zooms.front(), _zooms.back(),
                 (unsigned)_zooms.size());
    } else {
        ESP_LOGW("ViewMap", "No map tiles found on the SD card (/maps/*.mmap or /tiles/z/x/y.png)");
    }
}

bool ViewMap::ensureZoomAvailable() {
    if (_zooms.empty()) return false;
    if (std::find(_zooms.begin(), _zooms.end(), _zoomLevel) != _zooms.end()) {
        if (_usePack) _pack.selectZoom(_zoomLevel);
        return true;
    }
    _zoomLevel = _zooms[_zooms.size() / 2]; // middle level: usable detail, wide enough to orient
    if (_usePack) _pack.selectZoom(_zoomLevel);
    return true;
}

bool ViewMap::centerOnAvailableTiles() {
    if (_usePack) return centerOnPack();
    if (_zooms.empty()) return false;

    // Lowest level = widest coverage and by far the fewest tiles, so it can be scanned in full.
    const int z = _zooms.front();
    char zPath[64];
    snprintf(zPath, sizeof(zPath), "/tiles/%d", z);
    if (!SD_MMC.exists(zPath)) snprintf(zPath, sizeof(zPath), "/sdcard/tiles/%d", z);

    const int maxTile = 1 << z;
    const int sampleCap = 4096; // safety bound: a huge collection must not stall the tap
    int samples = 0;
    int firstX = 0, firstY = 0;
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
    bool hasFirst = false;

    File zd = SD_MMC.open(zPath);
    if (zd && zd.isDirectory()) {
        while (File xe = zd.openNextFile()) {
            int x = 0;
            const bool isXDir = xe.isDirectory() && parseInt(baseName(xe.name()), x) && x >= 0 && x < maxTile;
            xe.close();
            if (!isXDir || samples >= sampleCap) continue;

            char xPath[96];
            snprintf(xPath, sizeof(xPath), "%s/%d", zPath, x);
            File xd = SD_MMC.open(xPath);
            if (!xd || !xd.isDirectory()) {
                if (xd) xd.close();
                continue;
            }
            while (File ye = xd.openNextFile()) {
                int y = 0;
                const bool isTile = !ye.isDirectory() &&
                                    parseTileFileName(baseName(ye.name()), y) && y >= 0 && y < maxTile;
                ye.close();
                if (!isTile) continue;
                if (!hasFirst) {
                    hasFirst = true;
                    firstX = minX = maxX = x;
                    firstY = minY = maxY = y;
                } else {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
                if (++samples >= sampleCap) break;
            }
            xd.close();
        }
    }
    if (zd) zd.close();
    if (samples == 0) return false;

    double tx = (minX + maxX + 1) / 2.0;
    double ty = (minY + maxY + 1) / 2.0;
    const int chkX = (int)floor(tx);
    const int chkY = (int)floor(ty);
    if (chkX < 0 || chkY < 0 || chkX >= maxTile || chkY >= maxTile ||
        (!SD_MMC.exists(tilePath(z, chkX, chkY, false).c_str()) &&
         !SD_MMC.exists(tilePath(z, chkX, chkY, true).c_str()))) {
        tx = firstX + 0.5;
        ty = firstY + 0.5;
    }

    tileToLatLon(tx, ty, z, _centerLat, _centerLon);
    _selectedNodeNum = 0;
    ESP_LOGI("ViewMap", "Centred on the tile area: %.5f, %.5f (Z%d, %d samples)",
             _centerLat, _centerLon, z, samples);
    return true;
}

bool ViewMap::restoreSavedView() {
    int32_t latE7 = 0, lonE7 = 0;
    uint8_t zoom = 13;
    if (!StorageManager::getInstance().loadMapView(latE7, lonE7, zoom)) return false;
    _centerLat = (double)latE7 / 1e7;
    _centerLon = (double)lonE7 / 1e7;
    _zoomLevel = zoom;
    return true;
}

void ViewMap::saveView(bool force) {
    const uint32_t now = millis();
    if (!force && _lastSaveMs != 0 && (uint32_t)(now - _lastSaveMs) < 30000UL) return;
    _lastSaveMs = now;
    StorageManager::getInstance().saveMapView((int32_t)lround(_centerLat * 1e7),
                                              (int32_t)lround(_centerLon * 1e7),
                                              (uint8_t)_zoomLevel);
}

bool ViewMap::centerOnNode() {
    const auto& nodes = MeshService::getInstance().getNodes();
    const uint32_t local = MeshService::getInstance().getLocalNodeNum();
    for (const auto& n : nodes) {
        if (n.nodeNum == local && n.hasPosition && (n.lat != 0.0 || n.lon != 0.0)) {
            _centerLat = n.lat;
            _centerLon = n.lon;
            _selectedNodeNum = n.nodeNum;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- geo

double ViewMap::metersPerPixel() const {
    const double latRad = _centerLat * DEG2RAD;
    return 156543.03392 * cos(latRad) / pow(2.0, (double)_zoomLevel);
}

void ViewMap::latLonToTile(double lat, double lon, int z, double& tx, double& ty) {
    const double n = pow(2.0, z);
    const double latRad = lat * DEG2RAD;
    tx = (lon + 180.0) / 360.0 * n;
    ty = (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n;
}

void ViewMap::tileToLatLon(double tx, double ty, int z, double& lat, double& lon) {
    const double n = pow(2.0, z);
    lon = tx / n * 360.0 - 180.0;
    const double latRad = atan(sinh(M_PI * (1.0 - 2.0 * ty / n)));
    lat = latRad * RAD2DEG;
}

bool ViewMap::parseInt(const char* s, int& out) {
    if (!s || !*s) return false;
    int value = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10 + (*p - '0');
        if (value > 1000000) return false;
    }
    out = value;
    return true;
}

// Tile files are named "<y>.png": parse the digits and ignore the extension.
bool ViewMap::parseTileFileName(const char* name, int& out) {
    if (!name || !*name) return false;
    int value = 0;
    const char* p = name;
    for (; *p >= '0' && *p <= '9'; ++p) {
        value = value * 10 + (*p - '0');
        if (value > 1000000) return false;
    }
    if (p == name) return false;                       // no digits at all
    if (strncasecmp(p, ".png", 4) != 0) return false;  // not a tile file
    out = value;
    return true;
}

bool ViewMap::tileExistsAt(int z, double lat, double lon) {
    if (z < 1 || z > 19) return false;
    double tx = 0, ty = 0;
    latLonToTile(lat, lon, z, tx, ty);
    const int xi = (int)floor(tx);
    const int yi = (int)floor(ty);
    if (xi < 0 || yi < 0 || xi >= (1 << z) || yi >= (1 << z)) return false;
    if (_usePack) {
        const MapPack::ZoomInfo* info = _pack.findZoom(z);
        if (!info) return false;
        if ((uint32_t)xi < info->x0 || (uint32_t)xi >= info->x0 + info->nx) return false;
        if ((uint32_t)yi < info->y0 || (uint32_t)yi >= info->y0 + info->ny) return false;
        if (_pack.selectedZoom() == z) {
            uint32_t off = 0;
            uint16_t len = 0, flags = 0;
            return _pack.tileAt(xi, yi, off, len, flags);
        }
        return true; // inside the covered grid: good enough to pick the level
    }
    return SD_MMC.exists(tilePath(z, xi, yi, false).c_str()) ||
           SD_MMC.exists(tilePath(z, xi, yi, true).c_str());
}

// Snap to the available zoom closest to the wanted one that really covers the current centre.
bool ViewMap::pickZoomWithTile() {
    if (_zooms.empty()) return false;
    if (tileExistsAt(_zoomLevel, _centerLat, _centerLon)) return true;
    int best = _zoomLevel, bestDist = 1000;
    bool found = false;
    for (int z : _zooms) {
        if (!tileExistsAt(z, _centerLat, _centerLon)) continue;
        const int dist = abs(z - _zoomLevel);
        if (dist < bestDist) {
            bestDist = dist;
            best = z;
            found = true;
        }
    }
    if (found) {
        _zoomLevel = best;
        if (_usePack) _pack.selectZoom(_zoomLevel);
    }
    return found;
}

// BtnA (direction < 0) zooms in, BtnB zooms out. A step is only taken towards a level that covers
// the current centre: a level can exist on the card (another region, another download) and still be
// empty here, which used to leave the user on a white screen.
bool ViewMap::scrollPage(int8_t direction) {
    if (direction == 0 || _zooms.empty()) return false;
    int next = _zoomLevel;
    if (direction < 0) {
        for (int z : _zooms) {
            if (z > _zoomLevel && tileExistsAt(z, _centerLat, _centerLon)) { next = z; break; }
        }
    } else {
        for (int i = (int)_zooms.size() - 1; i >= 0; --i) {
            if (_zooms[i] < _zoomLevel && tileExistsAt(_zooms[i], _centerLat, _centerLon)) {
                next = _zooms[i];
                break;
            }
        }
    }
    if (next == _zoomLevel) return false; // no neighbour with a tile here: nothing to do
    _zoomLevel = next;
    if (_usePack) _pack.selectZoom(_zoomLevel);
    invalidateTileCache(); // another level: the cached tiles are for the previous one

    // Anteprima immediata: 1-bit, nessun flash. Si vede subito la mappa al nuovo zoom invece di
    // aspettare il debounce; il passaggio a 4 livelli (piu' lungo) resta programmato sotto.
    renderFull(false);

    // Debounce: non ridisegnare l'intera mappa a ogni click (es. z15 -> z12).
    // Attendi 1s di inattivita' prima di eseguire il refresh a 4 livelli di grigio.
    // renderFull(false) azzera _needsSettle, quindi il settle va riarmato dopo di lui.
    _dirtyFast = false;
    _needsSettle = true;
    _lastMoveMs = millis();
    _settleMs = ZOOM_SETTLE_MS;
    saveView(false);
    return true;
}

void ViewMap::panBy(int dx, int dy) {
    if ((dx == 0 && dy == 0) || !_canvasReady) return;
    const uint32_t tPan = millis();
    const bool logPan = (uint32_t)(tPan - _lastPanLogMs) > 500;
    const uint32_t readsBefore = _statTileReads;
    // A very fast flick can report a delta bigger than the viewport: clamp it so the
    // canvas shift stays a valid (if partial) translation.
    dx = std::max(-(480 - 1), std::min(480 - 1, dx));
    dy = std::max(-(CV_H - 1), std::min(CV_H - 1, dy));
    const double latRad = _centerLat * DEG2RAD;
    const double mpp = metersPerPixel();
    const double dEastMeters = -dx * mpp;
    const double dNorthMeters = dy * mpp;
    _centerLon += dEastMeters / (111320.0 * cos(latRad));
    _centerLat += dNorthMeters / 110540.0;
    if (_centerLat > 85.0) _centerLat = 85.0;
    if (_centerLat < -85.0) _centerLat = -85.0;
    while (_centerLon >= 180.0) _centerLon -= 360.0;
    while (_centerLon < -180.0) _centerLon += 360.0;

    // The map moves with the finger: shift what is already composed, then repaint
    // only the strips the shift uncovered. Repainting the bands is enough for the
    // *canvas* (the rest already holds the shifted image), but the panel cannot be
    // treated the same way: see the publish below.
    scrollCanvas(dx, dy);

    const int aw = abs(dx), ah = abs(dy);
    if (aw > 0) {
        const int x0 = dx > 0 ? 0 : 480 - aw;
        const int x1 = dx > 0 ? aw : 480;
        composeScene(x0, 0, x1, CV_H);
    }
    if (ah > 0) {
        const int y0 = dy > 0 ? 0 : CV_H - ah;
        const int y1 = dy > 0 ? ah : CV_H;
        composeScene(0, y0, 480, y1);
    }
    // The whole viewport must be republished. The e-paper keeps the image it was
    // given, it does not scroll its own glass: after a pan every pixel of the map
    // sits somewhere else, so updating only the newly exposed strip would leave the
    // old map in place next to a strip of new content (that is exactly the noise that
    // appeared while dragging). Partial publishes stay valid for UI updates where the
    // untouched part really does not change.
    publishRect(0, CV_H, false);
    _lastMoveMs = millis();
    _needsSettle = true;
    _settleMs = SETTLE_MS; // a pan settles after 1 s (a button zoom keeps its longer debounce)
    if (logPan) {
        _lastPanLogMs = millis();
        ESP_LOGI("ViewMap", "pan %d,%d: %u tiles read, refresh 480x%d, %u ms",
                 dx, dy, (unsigned)(_statTileReads - readsBefore), CV_H,
                 (unsigned)(millis() - tPan));
    }
}

// ---------------------------------------------------------------- composition

void ViewMap::composeScene(int x0, int y0, int x1, int y1) {
    if (!_canvasReady) return;
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(480, x1);
    y1 = std::min(CV_H, y1);
    if (x0 >= x1 || y0 >= y1) return;
    const bool full = (x0 == 0 && y0 == 0 && x1 == 480 && y1 == CV_H);
    const uint32_t tCompose = millis();
    const uint32_t readsBefore = _statTileReads;

    _canvas.setClipRect(x0, y0, x1 - x0, y1 - y0);
    _canvas.fillRect(x0, y0, x1 - x0, y1 - y0, TFT_WHITE);
    // Grid first, tiles on top: the grid only shows through where the card has no tile at this
    // zoom, so a partially covered area reads as "no map here" instead of a blank white band.
    drawGrid(false);

    const double latRad = _centerLat * DEG2RAD;
    const double mpp = metersPerPixel();
    const int maxTile = 1 << _zoomLevel;
    double tileX = 0, tileY = 0;
    latLonToTile(_centerLat, _centerLon, _zoomLevel, tileX, tileY);

    // Tile range that really covers the viewport.
    const int tx0 = (int)floor(tileX - (double)MAP_CX / TILE_SIZE);
    const int tx1 = (int)floor(tileX + (double)(480 - MAP_CX) / TILE_SIZE);
    const int ty0 = (int)floor(tileY - (double)(MAP_CY - MAP_TOP) / TILE_SIZE);
    const int ty1 = (int)floor(tileY + (double)(MAP_TILE_BOTTOM - MAP_CY) / TILE_SIZE);

    int drawn = 0;
    for (int ty = ty0; ty <= ty1; ++ty) {
        if (ty < 0 || ty >= maxTile) continue;
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (tx < 0 || tx >= maxTile) continue;
            const int sx = MAP_CX + (int)lround((tx - tileX) * TILE_SIZE);
            const int sy = CV_CY + (int)lround((ty - tileY) * TILE_SIZE);
            if (sx >= 480 || sy >= CV_H || sx + TILE_PX <= 0 || sy + TILE_PX <= 0) continue;
            const int slot = acquireTileSlot(_zoomLevel, tx, ty);
            if (slot < 0) continue;
            blitSlot(slot, sx, sy);
            ++drawn;
        }
    }

    drawScaleBar();
    drawNodes(latRad, mpp);
    drawWaypoints(latRad, mpp);
    drawActionButtons(_canvas);

    // Hint when there is really nothing to show. Only painted on a full compose: a
    // strip repaint must not re-draw a message that belongs to a different area.
    if (full && drawn == 0) {
        char hint[80];
        if (!StorageManager::getInstance().isSdMounted()) {
            snprintf(hint, sizeof(hint), "Insert the MicroSD with /maps or /tiles");
        } else if (_zooms.empty()) {
            snprintf(hint, sizeof(hint), "No map on SD: /maps/*.mmap");
        } else {
            if (_regions.size() > 1) {
                snprintf(hint, sizeof(hint), "No map here (Z %d..%d): long-press the top bar to change map",
                         _zooms.front(), _zooms.back());
            } else {
                snprintf(hint, sizeof(hint), "No map here (Z %d..%d)", _zooms.front(), _zooms.back());
            }
        }
        _canvas.setTextColor(TFT_BLACK, TFT_WHITE);
        _canvas.setTextDatum(textdatum_t::top_center);
        _canvas.setTextSize(1);
        _canvas.drawString(hint, MAP_CX, 6);
    }
    if (_toastUntilMs && (int32_t)(millis() - _toastUntilMs) < 0) {
        _canvas.setTextDatum(textdatum_t::top_left);
        _canvas.setTextSize(1);
        _canvas.fillRect(6, 6, 320, 18, TFT_WHITE);
        _canvas.drawRect(6, 6, 320, 18, TFT_BLACK);
        _canvas.setTextColor(TFT_BLACK, TFT_WHITE);
        _canvas.drawString(_toast, 12, 9);
    }
    _canvas.clearClipRect();
    if (full) {
        ESP_LOGI("ViewMap", "composition: %d tiles, %u read from SD, %u ms (cache %u)",
                 drawn, (unsigned)(_statTileReads - readsBefore),
                 (unsigned)(millis() - tCompose), (unsigned)_statTileReads);
    }
}

// ---------------------------------------------------------------- touch

bool ViewMap::handleTouch(const TouchEvent& ev) {
    // Taps on the controls (header button, FABs, info card) - only when the finger did not drag
    if (ev.type == TouchEventType::Click || ev.type == TouchEventType::Up) {
        const int16_t dx = ev.x - ev.startX;
        const int16_t dy = ev.y - ev.startY;
        const bool isTap = (abs(dx) < 25 && abs(dy) < 25);

        if (isTap && ev.startY >= MAP_VIEW_Y && ev.startY < MAP_TOP) {
            if (ev.durationMs >= 600) {
                // Long press: switch region (only meaningful when the card holds several .mmap files)
                if (_regions.size() > 1) {
                    openRegion((_regionIndex + 1) % _regions.size(), true);
                    centerOnPack();
                } else if (_usePack) {
                    centerOnPack();
                    toast("%s", _pack.name().c_str());
                } else {
                    toast("No other map");
                }
                ensureZoomAvailable();
                pickZoomWithTile();
                invalidateTileCache();
                requestFastRedraw();
                return true;
            }
            return true;
        }

        if (isTap && ev.startX >= FAB_X && ev.startX <= FAB_X + FAB_W) {
            if (ev.startY >= FAB_LOC_Y && ev.startY <= FAB_LOC_Y + FAB_H) {
                // "My location": where I am, or - if unknown - where the maps are
                if (!centerOnNode()) centerOnAvailableTiles();
                ensureZoomAvailable();
                pickZoomWithTile();
                requestFastRedraw();
                saveView(true);
                return true;
            }
            if (ev.startY >= FAB_PIN_Y && ev.startY <= FAB_PIN_Y + FAB_H) {
                MeshService::getInstance().updateLocalPosition(_centerLat, _centerLon, 0);
                MeshService::getInstance().broadcastPosition();
                MeshService::getInstance().saveConfig();
                BSP::getInstance().click();
                requestFastRedraw();
                saveView(true);
                return true;
            }
        }

        if (isTap && ev.startY >= CARD_Y && ev.startY <= CARD_Y + CARD_H) {
            const auto& nodes = MeshService::getInstance().getNodes();
            if (!nodes.empty()) {
                size_t idx = 0;
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i].nodeNum == _selectedNodeNum) {
                        idx = (i + 1) % nodes.size();
                        break;
                    }
                }
                _selectedNodeNum = nodes[idx].nodeNum;
                if (nodes[idx].hasPosition) {
                    _centerLat = nodes[idx].lat;
                    _centerLon = nodes[idx].lon;
                }
                requestFastRedraw();
                saveView(false);
                return true;
            }
        }
    }

    // Drag: incremental panning while the finger moves (both axes, no more 80 px jumps).
    // Publishes are coalesced: one panel update every MIN_PUBLISH_MS (or PUBLISH_STEP_PX of
    // movement), because the e-paper refresh - not the composition - is the limit.
    if (ev.type == TouchEventType::Down) {
        _dragging = true;
        _dragX = ev.x;
        _dragY = ev.y;
        _pendingDx = _pendingDy = 0;
        return false;
    }
    if (ev.type == TouchEventType::Move) {
        if (!_dragging) {
            _dragging = true;
            _dragX = ev.startX;
            _dragY = ev.startY;
        }
        const int dx = ev.x - _dragX;
        const int dy = ev.y - _dragY;
        if (dx != 0 || dy != 0) {
            _dragX = ev.x;
            _dragY = ev.y;
            _pendingDx += dx;
            _pendingDy += dy;
            const bool bigStep = abs(_pendingDx) >= PUBLISH_STEP_PX || abs(_pendingDy) >= PUBLISH_STEP_PX;
            const bool timeUp = (uint32_t)(millis() - _lastPublishMs) >= MIN_PUBLISH_MS;
            if (bigStep || timeUp) {
                const int pdx = _pendingDx, pdy = _pendingDy;
                _pendingDx = _pendingDy = 0;
                _lastPublishMs = millis();
                panBy(pdx, pdy);
            }
        }
        return true;
    }

    // Release: a flick that produced no Move events still pans once by the whole delta
    if (ev.type == TouchEventType::Up || ev.type == TouchEventType::SwipeLeft || ev.type == TouchEventType::SwipeRight ||
        ev.type == TouchEventType::SwipeUp || ev.type == TouchEventType::SwipeDown) {
        int dx = ev.x - ev.startX;
        int dy = ev.y - ev.startY;
        const bool wasDragging = _dragging;
        _dragging = false;
        if (_pendingDx != 0 || _pendingDy != 0) {
            dx = _pendingDx;
            dy = _pendingDy;
            _pendingDx = _pendingDy = 0;
            panBy(dx, dy);
            return true;
        }
        if (!wasDragging && (abs(dx) >= 15 || abs(dy) >= 15)) {
            // Fast flick that never produced Move events: pan once by the whole delta. panBy() also
            // (re)arms the settling pass, which is what ends the 1-bit phase.
            panBy(dx, dy);
        }
        return (abs(dx) >= 15 || abs(dy) >= 15);
    }

    return false;
}

void ViewMap::draw(M5GFX& gfx) {
    if (!ensureCanvas()) return;
    drawHeader(gfx);
    composeScene(0, 0, 480, CV_H);
    // Put the composed map into the panel frame buffer: the full-screen refresh that
    // follows draw() sends it together with the rest of the UI.
    M5.Display.pushImage(0, MAP_TOP, 480, CV_H, (const lgfx::grayscale_t*)_canvas.getBuffer());
    // NOTE: the action buttons live *inside* the canvas (composeScene draws them), so they must not
    // be drawn again here: drawActionButtons() works in canvas coordinates and doing it on the
    // display painted a second, 78 px higher copy of both buttons (the "duplicated buttons" glitch
    // seen right after waking up from standby).
    drawInfoCard(gfx);
    _dirtyFast = false;
    _needsSettle = false;
}

// ---------------------------------------------------------------- drawing

void ViewMap::drawHeader(M5GFX& gfx) {
    // Dot-screen bar (black text) like the other view headers.
    headerBackground(gfx, MAP_VIEW_Y, HDR_H);
    gfx.drawFastHLine(0, MAP_TOP - 1, 480, TFT_BLACK);

    gfx.setTextColor(TFT_BLACK);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(2);
    char title[32];
    snprintf(title, sizeof(title), "MAPS  Z:%d", _zoomLevel);
    gfx.drawString(title, 18, MAP_VIEW_Y + HDR_H / 2);

    // Centre coordinates in the same font/size as the zoom label
    char coords[40];
    snprintf(coords, sizeof(coords), "%.4f, %.4f", _centerLat, _centerLon);
    gfx.setTextDatum(textdatum_t::middle_right);
    gfx.drawString(coords, 466, MAP_VIEW_Y + HDR_H / 2);
}

void ViewMap::drawGrid(bool withCrosshair) {
    for (int x = 40; x < 480; x += 80) _canvas.drawFastVLine(x, 1, CV_H - 1, TFT_LIGHTGRAY);
    for (int y = 22; y < CV_TILE_BOTTOM; y += 80) _canvas.drawFastHLine(0, y, 480, TFT_LIGHTGRAY);

    if (!withCrosshair) return;
    _canvas.drawFastHLine(MAP_CX - 15, CV_CY, 31, TFT_BLACK);
    _canvas.drawFastVLine(MAP_CX, CV_CY - 15, 31, TFT_BLACK);
    _canvas.drawCircle(MAP_CX, CV_CY, 8, TFT_BLACK);
}

void ViewMap::drawScaleBar() {
    const double scaleMeters = metersPerPixel() * 100.0;
    char scaleStr[24];
    if (scaleMeters >= 1000.0) snprintf(scaleStr, sizeof(scaleStr), "%.1f km", scaleMeters / 1000.0);
    else snprintf(scaleStr, sizeof(scaleStr), "%.0f m", scaleMeters);

    const int y = CV_TILE_BOTTOM - 14;
    _canvas.drawFastHLine(20, y, 100, TFT_BLACK);
    _canvas.drawFastVLine(20, y - 6, 13, TFT_BLACK);
    _canvas.drawFastVLine(120, y - 6, 13, TFT_BLACK);
    _canvas.setTextColor(TFT_BLACK, TFT_WHITE);
    _canvas.setTextDatum(textdatum_t::bottom_center);
    _canvas.setTextSize(1);
    _canvas.drawString(scaleStr, 70, y - 6);
}

void ViewMap::drawNodes(double latRad, double mpp) {
    const auto& nodes = MeshService::getInstance().getNodes();
    const uint32_t local = MeshService::getInstance().getLocalNodeNum();

    for (const auto& n : nodes) {
        if (!n.hasPosition || (n.lat == 0.0 && n.lon == 0.0)) continue;
        const double dEastMeters = (n.lon - _centerLon) * 111320.0 * cos(latRad);
        const double dNorthMeters = (n.lat - _centerLat) * 110540.0;
        const int px = MAP_CX + (int)lround(dEastMeters / mpp);
        const int py = CV_CY - (int)lround(dNorthMeters / mpp);
        if (px < -20 || px > 500 || py < -20 || py > CV_TILE_BOTTOM + 20) continue;

        const bool isSelected = (_selectedNodeNum == n.nodeNum);
        _canvas.setTextColor(TFT_BLACK, TFT_WHITE);
        _canvas.setTextDatum(textdatum_t::top_center);
        _canvas.setTextSize(1);

        if (n.nodeNum == local) {
            if (isSelected) {
                _canvas.drawCircle(px, py, 16, TFT_BLACK);
                _canvas.drawCircle(px, py, 17, TFT_BLACK);
            }
            drawMyLocationIcon(_canvas, px, py);
            _canvas.drawString("YOU", px, py + 14);
        } else {
            if (isSelected) {
                _canvas.drawCircle(px, py, 14, TFT_BLACK);
                _canvas.drawCircle(px, py, 15, TFT_BLACK);
            }
            _canvas.fillCircle(px, py, 8, TFT_DARKGRAY);
            _canvas.fillCircle(px, py, 4, TFT_WHITE);
            char label[20];
            snprintf(label, sizeof(label), "%s", n.shortName);
            _canvas.drawString(label, px, py + 10);
        }
    }
}

void ViewMap::drawWaypoints(double latRad, double mpp) {
    const uint32_t nowUnix = BSP::getInstance().getRtcUnix();
    for (const auto& wp : MeshService::getInstance().getWaypoints()) {
        if (wp.expire != 0 && nowUnix > 1700000000UL && wp.expire < nowUnix) continue; // expired

        const double dEastMeters = (wp.lon - _centerLon) * 111320.0 * cos(latRad);
        const double dNorthMeters = (wp.lat - _centerLat) * 110540.0;
        const int px = MAP_CX + (int)lround(dEastMeters / mpp);
        const int py = CV_CY - (int)lround(dNorthMeters / mpp);
        if (px < 0 || px > 480 || py < 0 || py > CV_TILE_BOTTOM) continue;

        _canvas.fillRect(px - 6, py - 6, 12, 12, TFT_BLACK);
        _canvas.fillRect(px - 3, py - 3, 6, 6, TFT_WHITE);
        if (wp.name[0] != '\0') {
            _canvas.setTextColor(TFT_BLACK, TFT_WHITE);
            _canvas.setTextDatum(textdatum_t::top_center);
            _canvas.setTextSize(1);
            char wlabel[16];
            snprintf(wlabel, sizeof(wlabel), "%.14s", wp.name);
            _canvas.drawString(wlabel, px, py + 8);
        }
    }
}

void ViewMap::drawMyLocationIcon(lgfx::LGFXBase& gfx, int cx, int cy) {
    // ~25% bigger than before: the icons were hard to read on the e-paper
    gfx.drawCircle(cx, cy, 13, TFT_BLACK);
    gfx.drawCircle(cx, cy, 14, TFT_BLACK);
    gfx.fillCircle(cx, cy, 4, TFT_BLACK);
    gfx.drawFastVLine(cx, cy - 20, 6, TFT_BLACK);
    gfx.drawFastVLine(cx, cy + 15, 6, TFT_BLACK);
    gfx.drawFastHLine(cx - 20, cy, 6, TFT_BLACK);
    gfx.drawFastHLine(cx + 15, cy, 6, TFT_BLACK);
}

void ViewMap::drawMapPin(lgfx::LGFXBase& gfx, int cx, int cy, bool solid) {
    const int topY = cy - 6;
    if (solid) {
        gfx.fillCircle(cx, topY, 12, TFT_BLACK);
        gfx.fillTriangle(cx - 12, topY + 1, cx + 12, topY + 1, cx, cy + 17, TFT_BLACK);
        gfx.fillCircle(cx, topY, 5, TFT_WHITE);
    } else {
        gfx.drawCircle(cx, topY, 12, TFT_BLACK);
        gfx.drawCircle(cx, topY, 11, TFT_BLACK);
        gfx.drawLine(cx - 11, topY + 2, cx, cy + 17, TFT_BLACK);
        gfx.drawLine(cx - 10, topY + 2, cx, cy + 16, TFT_BLACK);
        gfx.drawLine(cx + 11, topY + 2, cx, cy + 17, TFT_BLACK);
        gfx.drawLine(cx + 10, topY + 2, cx, cy + 16, TFT_BLACK);
        gfx.drawCircle(cx, topY, 4, TFT_BLACK);
    }
}

void ViewMap::drawActionButtons(lgfx::LGFXBase& gfx) {
    const auto& nodes = MeshService::getInstance().getNodes();
    const uint32_t local = MeshService::getInstance().getLocalNodeNum();
    bool localPosSet = false;
    for (const auto& n : nodes) {
        if (n.nodeNum == local && n.hasPosition && (n.lat != 0.0 || n.lon != 0.0)) {
            localPosSet = true;
            break;
        }
    }
    const int yLoc = FAB_LOC_Y - MAP_TOP;
    const int yPin = FAB_PIN_Y - MAP_TOP;

    // My location (crosshair) / set position here (pin)
    gfx.fillRoundRect(FAB_X, yLoc, FAB_W, FAB_H, 8, TFT_WHITE);
    gfx.drawRoundRect(FAB_X, yLoc, FAB_W, FAB_H, 8, TFT_BLACK);
    gfx.drawRoundRect(FAB_X + 1, yLoc + 1, FAB_W - 2, FAB_H - 2, 7, TFT_BLACK);
    drawMyLocationIcon(gfx, FAB_X + FAB_W / 2, yLoc + FAB_H / 2);

    gfx.fillRoundRect(FAB_X, yPin, FAB_W, FAB_H, 8, TFT_WHITE);
    gfx.drawRoundRect(FAB_X, yPin, FAB_W, FAB_H, 8, TFT_BLACK);
    gfx.drawRoundRect(FAB_X + 1, yPin + 1, FAB_W - 2, FAB_H - 2, 7, TFT_BLACK);
    drawMapPin(gfx, FAB_X + FAB_W / 2, yPin + FAB_H / 2, localPosSet);
}

void ViewMap::drawInfoCard(M5GFX& gfx) {
    const auto& nodes = MeshService::getInstance().getNodes();
    const MeshNode* sel = MeshService::getInstance().findNode(_selectedNodeNum);
    if (!sel && !nodes.empty()) sel = &nodes[0];

    gfx.drawRoundRect(10, CARD_Y, 460, CARD_H, 6, TFT_BLACK);
    gfx.fillRoundRect(12, CARD_Y + 2, 456, CARD_H - 4, 4, TFT_WHITE);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(1);

    if (!sel) {
        gfx.drawString(_usePack ? _pack.name().c_str() : "No mesh node selected",
                       22, CARD_Y + CARD_H / 2);
        return;
    }

    char line1[96];
    snprintf(line1, sizeof(line1), "Node: %s (%s) | SNR: %.1f dB | RSSI: %.0f dBm",
             sel->shortName, sel->longName, sel->snr, sel->rssi);
    gfx.drawString(line1, 22, CARD_Y + 20);

    // Second line: map credits instead of the node coordinates (asked for explicitly). The .mmap
    // container carries its own attribution; the PNG tree is the OSM "Toner" style.
    char line2[96];
    if (_usePack && !_pack.attribution().empty()) {
        snprintf(line2, sizeof(line2), "%.90s", _pack.attribution().c_str());
    } else {
        snprintf(line2, sizeof(line2), "Map data (c) OpenStreetMap contributors | Toner style (Stamen)");
    }
    gfx.drawString(line2, 22, CARD_Y + 42);
}

} // namespace MonoMesh
