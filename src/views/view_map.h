#pragma once

#include "view_base.h"
#include "../map_pack.h"
#include "../map_vector.h"
#include <M5GFX.h>
#include <string>
#include <vector>

namespace MonoMesh {

// Offline map view.
//
// Rendering model (this is what makes panning fast):
//  * the whole map area lives in one 8-bit greyscale canvas in PSRAM;
//  * tiles come from a packed .mmap container (see map_pack.h) or, as a
//    fallback, from the legacy /tiles/z/x/y.png tree;
//  * dragging shifts the canvas and republishes to the panel only the strip
//    that the shift uncovered, instead of the full 480x583 area;
//  * the 4-level "settling" pass ~1 s after the finger stops still refreshes the
//    whole area (that is how the panel's half-mode update works).
class ViewMap : public ViewBase {
public:
    ViewMap();
    ~ViewMap() override;

    void onEnter() override;
    void draw(M5GFX& gfx) override;
    bool handleTouch(const TouchEvent& ev) override;
    void update() override;
    bool scrollPage(int8_t direction) override;
    // Jump to the map centred on a node (used by "VIEW ON MAP" in the node list). The request is
    // applied when the map view is entered, with the given zoom (clamped to the available levels).
    static void focusNodeOnMap(uint32_t nodeNum, int zoom);

private:
    // ---- geometry, shared by draw() and handleTouch() so the hit boxes cannot drift ----
    static constexpr int MAP_VIEW_Y = 44;              // the status bar occupies 0..43
    static constexpr int HDR_H = 32;                   // map header: 44..75, line at 76 like every view
    static constexpr int MAP_TOP = MAP_VIEW_Y + HDR_H + 1; // first tile row: 77, flush under the header line
    static constexpr int MAP_BOTTOM = 729;             // nav bar starts at 730
    static constexpr int MAP_H = MAP_BOTTOM - MAP_TOP + 1; // map area + info card (flush range)
    static constexpr int MAP_CX = 240;
    static constexpr int MAP_CY = 368;                 // centre of the free area 77..659
    static constexpr int MAP_TILE_BOTTOM = 659;        // info card starts at 660
    static constexpr int CARD_Y = 660;
    static constexpr int CARD_H = 60;
    static constexpr int FAB_X = 412;
    static constexpr int FAB_W = 54;
    static constexpr int FAB_H = 54;
    static constexpr int FAB_LOC_Y = 540;              // "my location"
    static constexpr int FAB_PIN_Y = 600;              // "set position here"
    static constexpr int TILE_PX = 256;
    static constexpr int TILE_SLOTS = 24;              // decoded tiles kept in RAM
    // Web Mercator levels this view shifts safely. A zoom byte read from a container
    // (or from NVS) outside this range would make "1 << z" undefined in composeScene().
    static constexpr int ZOOM_MIN = 1;
    static constexpr int ZOOM_MAX = 19;
    static constexpr uint32_t MISS_RETRY_MS = 5000;    // re-probe a cached miss after this long
    static constexpr uint32_t SETTLE_MS = 1000;        // 1-bit while moving, gray once you stop
    static constexpr uint32_t ZOOM_SETTLE_MS = 1000;   // 1s debounce before 4-level grayscale refresh
    static constexpr uint32_t MIN_PUBLISH_MS = 60;     // coalesce drag events into one panel update
    static constexpr int PUBLISH_STEP_PX = 24;         // ... or publish early after this much drag

    // Canvas space: same as screen space but with the origin on the first map row.
    // The canvas stops at the last map row: the info card below (CARD_Y..) is drawn
    // straight on the display, so a pan publish can never overwrite it.
    static constexpr int CV_H = MAP_TILE_BOTTOM - MAP_TOP + 1;
    static constexpr int CV_CY = MAP_CY - MAP_TOP;
    static constexpr int CV_TILE_BOTTOM = MAP_TILE_BOTTOM - MAP_TOP;

    enum SlotState : uint8_t {
        SLOT_EMPTY = 0,
        SLOT_PACKED,        // packed monochrome bits from the container
        SLOT_OVERZOOM,      // packed bits of a parent tile, blitted scaled up
        SLOT_HATCH,         // no data anywhere: diagonal "out of coverage" pattern
        SLOT_PNG,           // legacy PNG bytes
        SLOT_BLANK_WHITE,   // tile exists but is empty: fill with white
        SLOT_BLANK_BLACK,   // ... or black (dark style)
        SLOT_MISSING,       // no tile here: let the grid show through
    };

    struct TileSlot {
        int z = 0, x = 0, y = 0;
        uint32_t lastUse = 0;
        uint32_t missMs = 0;     // when a miss got cached, so a transient SD error can be retried
        uint8_t state = SLOT_EMPTY;
        uint8_t* data = nullptr; // PSRAM: packed bits or PNG bytes, depending on the state
        size_t len = 0;
        uint8_t overzoom = 0;    // SLOT_OVERZOOM: 2^overzoom = scale (parent -> child)
        uint8_t srcBits = 1;     // SLOT_OVERZOOM: packed bits of the parent tile
        uint16_t srcPx = 0;      // SLOT_OVERZOOM: parent tile size in pixels
        bool fromBasemap = false; // pixels come from the global basemap: base style (25% grey)
    };

    // ---- region / container ----
    MapPack _pack;
    // Vector container (.mmap v2, plan Piano B): rendered on device by the
    // shared libmapcore rasteriser instead of blitting pre-rendered tiles.
    // Raster v1 containers and the basemap keep using MapPack unchanged.
    MapVector _vector;
    // Plan D5 label layout cache: the accepted set is world anchored, so a
    // strip repaint inside the slack redraws it without touching the POI
    // sections; full renders rebuild it with identical placement rules.
    mm_label_cache _labelCache = {};
    bool _useVector = false;
    // Global z0-7 courtesy basemap (optional): the fallback for areas no region
    // covers, opened on its own so it never disturbs the region selection.
    MapPack _basemap;
    bool _useBasemap = false;
    static constexpr int kOverzoomLevels = 4;   // parent levels tried by the fallback
    std::vector<std::string> _regions;
    size_t _regionIndex = 0;
    bool _usePack = false;
    char _toast[48] = {0};
    uint32_t _toastUntilMs = 0;

    // ---- composition buffer ----
    M5Canvas _canvas{&M5.Display};
    bool _canvasReady = false;
    // Fill/label workspace for the vector rasteriser (plan D6 arena row).
    uint8_t* _vecScratch = nullptr;

    double _centerLat = 45.4642; // only until a real position (or the tile area) is known
    double _centerLon = 9.1900;
    int _zoomLevel = 13;
    uint32_t _selectedNodeNum = 0;
    bool _showNodeInfo = true;

    std::vector<int> _zooms;     // zoom levels really present on the SD card (ascending)
    bool _sdWasMounted = false;
    bool _viewInitialised = false;

    TileSlot _tileCache[TILE_SLOTS];
    uint32_t _tileUseCounter = 0;

    bool _dirtyFast = false;     // the map area needs a 1-bit refresh
    bool _needsSettle = false;   // the map area needs the grayscale "finishing" pass
    // The composed canvas was pushed into the panel buffer and nothing redrew
    // it since: the settle pass can reuse the panel buffer and skip the whole
    // composition (one full vector render per pan/zoom was pure waste).
    bool _canvasPushed = false;
    uint32_t _lastMoveMs = 0;
    uint32_t _settleMs = SETTLE_MS;
    uint32_t _lastSaveMs = 0;

    static uint32_t s_focusNodeNum; // pending "show this node" request coming from the node list
    static int s_focusZoom;
    uint32_t _lastPublishMs = 0;
    uint32_t _statTileReads = 0;   // tiles really read from the SD (diagnostics)
    uint32_t _lastPanLogMs = 0;

    bool _dragging = false;
    int16_t _dragX = 0, _dragY = 0;
    int16_t _pendingDx = 0, _pendingDy = 0;

    double metersPerPixel() const;

    // ---- composition ----
    bool ensureCanvas();
    void releaseCanvas();
    void composeScene(int x0, int y0, int x1, int y1);
    // Vector path: draws the map layers + labels into the canvas clip rect with
    // the shared libmapcore rasteriser (strip repaints are just narrow clips).
    void composeVectorScene(int x0, int y0, int x1, int y1);
    void publishRect(int canvasY0, int canvasY1, bool grayscale);
    void scrollCanvas(int dx, int dy);
    void renderFull(bool grayscale);
    // Grayscale "settling" pass: reuses the panel buffer when the canvas has
    // not changed since the fast pass, recomposes only when it must.
    void settleGrayscale();
    void toast(const char* fmt, ...);
    // Called by StorageManager right before the FAT is unmounted (standby, power off):
    // the container file must not stay open across SD_MMC.end(). Public because the
    // storage singleton calls it through a plain function pointer.
public:
    void releaseSdHandles();
private:
    static void sdReleaseHook(void* self);

    void drawHeader(M5GFX& gfx);
    void drawGrid(bool withCrosshair);
    // Screen-fixed UI (scale bar, action buttons, toast): drawn on the display
    // after every canvas push, never inside the scrollable canvas.
    void drawFixedOverlays(M5GFX& gfx);
    void drawScaleBar(lgfx::LGFXBase& gfx);
    void drawNodes(double latRad, double mpp);
    void drawWaypoints(double latRad, double mpp);
    void drawInfoCard(M5GFX& gfx);
    void drawActionButtons(lgfx::LGFXBase& gfx);
    void drawMyLocationIcon(lgfx::LGFXBase& gfx, int cx, int cy);
    void drawMapPin(lgfx::LGFXBase& gfx, int cx, int cy, bool solid);

    // Tiles: a tile is read into RAM completely before being decoded or blitted, so a slow or
    // failed SD read can never leave half a tile on the panel, and the bytes stay cached to keep
    // dragging off the SD.
    // The SD is mounted on "/sdcard" and the FS API prepends the mount point, so the working path is
    // "/tiles/z/x/y.png": legacyRoot=true only covers cards with a real nested "sdcard" folder.
    std::string tilePath(int z, int x, int y, bool legacyRoot) const;
    int acquireTileSlot(int z, int x, int y);
    bool loadSlot(int slot, int z, int x, int y);
    void blitSlot(int slot, int canvasX, int canvasY);
    // Fallback for a tile no container has: a parent level of the same
    // container, then the basemap, then the hatch (see loadSlot).
    void openBasemap();
    bool loadOverzoom(int slot, MapPack& pack, bool primary, int z, int x, int y,
                      int maxLevels);
    // Small ring cache for the parent lookups of loadOverzoom: panning along a
    // coverage edge asks the same parent tile for several neighbouring children,
    // and a hit skips the presence-row reads on the SD.  Single-threaded view.
    struct ParentHit {
        int z = -1, x = 0, y = 0;
        bool present = false;
        uint32_t offset = 0;
        uint16_t length = 0, flags = 0;
    };
    ParentHit _parentHits[8];
    uint8_t _parentHitNext = 0;
    bool tileAtZoomCached(int z, int x, int y, uint32_t& offset, uint16_t& length,
                          uint16_t& flags);
    void clearParentHits();

    // Web Mercator helpers (same scheme as /tiles/z/x/y.png)
    static void latLonToTile(double lat, double lon, int z, double& tx, double& ty);
    static void tileToLatLon(double tx, double ty, int z, double& lat, double& lon);
    // Rejects NaN, ±inf and anything outside the Web Mercator range this view can draw:
    // node positions arrive from the mesh and a container/NVS can carry anything.
    static bool validMapCenter(double lat, double lon);
    static bool parseInt(const char* s, int& out);
    static bool parseTileFileName(const char* name, int& out);

    void scanZooms();
    void invalidateTileCache();
    bool ensureZoomAvailable();
    bool tileExistsAt(int z, double lat, double lon);
    bool pickZoomWithTile();
    bool centerOnNode();
    void applyPendingFocus();
    bool centerOnAvailableTiles();
    bool centerOnPack();
    bool openRegion(size_t index, bool announce);
    void scanRegions();
    bool restoreSavedView();
    void saveView(bool force);

    void requestFastRedraw(uint32_t settleMs = SETTLE_MS);
    void panBy(int dx, int dy);
};

} // namespace MonoMesh
