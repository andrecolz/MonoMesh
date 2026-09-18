#pragma once

#include <Arduino.h>
#include <M5Unified.h>

namespace MonoMesh {

enum class RefreshMode {
    FastPartial, // 1-bit LUT for keyboard, buttons, scrolls (<150ms)
    Grayscale,   // 2-bit 4-level gray for static, radar, backgrounds
    FullClear    // Full anti-ghosting waveform refresh
};

class EPDDriver {
public:
    static EPDDriver& getInstance() {
        static EPDDriver instance;
        return instance;
    }

    bool init();

    void setRefreshMode(RefreshMode mode);
    RefreshMode getRefreshMode() const { return _currentMode; }

    // Flush whole display
    void flush();

    // Partial flush of a specific bounding box (for keyboard, status icon, etc.)
    // countPartial=false keeps the update out of the anti-ghosting counter: the map needs many
    // flash-free 1-bit updates while dragging and a single grayscale pass when it settles.
    void flushRect(int32_t x, int32_t y, int32_t w, int32_t h, bool countPartial = true);
    // Partial flush of a bounding box using the 2-bit (4 gray levels) waveform: used as the
    // "settling" pass of the map, after the fast 1-bit updates.
    void flushRectGrayscale(int32_t x, int32_t y, int32_t w, int32_t h);

    // Force full clear anti-ghosting refresh
    void fullClear();
    // Drop the accumulated partial-refresh count (e.g. when entering standby) so the next partial
    // update does not trigger a full anti-ghosting clear in the middle of a session.
    void resetPartialCounter();

    // Threshold configuration for auto-full-clear
    void setAntiGhostingThreshold(uint16_t count) { _antiGhostThreshold = count; }
    uint16_t getAntiGhostingThreshold() const { return _antiGhostThreshold; }
    uint16_t getPartialRefreshCount() const { return _partialRefreshCount; }

    M5GFX& getDisplay() { return M5.Display; }

private:
    EPDDriver() = default;
    ~EPDDriver() = default;

    RefreshMode _currentMode = RefreshMode::Grayscale;
    uint16_t _partialRefreshCount = 0;
    uint16_t _antiGhostThreshold = 0; // 0 = disabled (manual only, avoids unexpected screen flashes)

    void applyEpdMode(m5gfx::epd_mode_t mode);
};

} // namespace MonoMesh
