#include "epd_driver.h"
#include <esp_log.h>

static constexpr const char* TAG = "MonoMesh-EPD";

namespace MonoMesh {

// Map the driver mode to the M5GFX LUT. Keeping the mapping in one place lets the flush paths
// re-assert the driver mode on the display instead of trusting M5.Display.getEpdMode(), which is
// stale after the direct setEpdMode(epd_quality) calls of ClockMode/BSP shutdown screens.
static m5gfx::epd_mode_t gfxModeFor(RefreshMode mode) {
    switch (mode) {
        case RefreshMode::FastPartial: return m5gfx::epd_mode_t::epd_fastest;
        case RefreshMode::Grayscale:   return m5gfx::epd_mode_t::epd_text;
        case RefreshMode::FullClear:   return m5gfx::epd_mode_t::epd_quality;
    }
    return m5gfx::epd_mode_t::epd_text;
}

// Intersect a rectangle with the panel without ever computing x+w in 32-bit: both values come from
// a public API, so a caller (or a future one) can pass extremes that would overflow the addition.
static bool clipToPanel(int32_t& x, int32_t& y, int32_t& w, int32_t& h) {
    const int64_t screenW = (int64_t)M5.Display.width();
    const int64_t screenH = (int64_t)M5.Display.height();
    int64_t x0 = x, y0 = y;
    int64_t x1 = (int64_t)x + (int64_t)w;
    int64_t y1 = (int64_t)y + (int64_t)h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > screenW) x1 = screenW;
    if (y1 > screenH) y1 = screenH;
    if (x0 >= x1 || y0 >= y1) return false;
    x = (int32_t)x0;
    y = (int32_t)y0;
    w = (int32_t)(x1 - x0);
    h = (int32_t)(y1 - y0);
    return true;
}

bool EPDDriver::init() {
    ESP_LOGI(TAG, "Initializing SSD1677 Display Controller...");
    M5.Display.setRotation(0);
    M5.Display.setAutoDisplay(false);

    setRefreshMode(RefreshMode::Grayscale);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.display();
    M5.Display.waitDisplay();

    ESP_LOGI(TAG, "EPD initialized: %dx%d", M5.Display.width(), M5.Display.height());
    if (M5.Display.width() != 480 || M5.Display.height() != 800) {
        // Every layout in the firmware is hardcoded for the 480x800 panel: report it instead of
        // drawing garbage that looks like a software bug.
        ESP_LOGE(TAG, "Unexpected panel geometry (expected 480x800)");
        return false;
    }
    return true;
}

void EPDDriver::applyEpdMode(m5gfx::epd_mode_t mode) {
    M5.Display.setEpdMode(mode);
}

void EPDDriver::setRefreshMode(RefreshMode mode) {
    _currentMode = mode;
    applyEpdMode(gfxModeFor(mode));
}

void EPDDriver::flush() {
    if (_currentMode == RefreshMode::FastPartial) {
        _partialRefreshCount++;
        if (_antiGhostThreshold > 0 && _partialRefreshCount >= _antiGhostThreshold) {
            fullClear();
            return;
        }
    } else {
        _partialRefreshCount = 0;
    }

    // The driver mode is authoritative: a direct setEpdMode() elsewhere must not make this full
    // update use the wrong waveform.
    M5.Display.setEpdMode(gfxModeFor(_currentMode));
    M5.Display.display();
}

void EPDDriver::flushRect(int32_t x, int32_t y, int32_t w, int32_t h, bool countPartial) {
    if (!clipToPanel(x, y, w, h)) return;

    if (_currentMode == RefreshMode::FastPartial) {
        if (countPartial) {
            _partialRefreshCount++;
            if (_antiGhostThreshold > 0 && _partialRefreshCount >= _antiGhostThreshold) {
                fullClear();
                return;
            }
        }
    } else if (countPartial) {
        // flushRect always forces the 1-bit LUT below, i.e. it IS a partial refresh whatever the
        // current mode is: count it too, or the anti-ghosting policy differs from the grayscale path.
        _partialRefreshCount++;
        if (_antiGhostThreshold > 0 && _partialRefreshCount >= _antiGhostThreshold) {
            fullClear();
            return;
        }
    }

    // Always enforce 1-bit fast LUT for rectangular partial updates to prevent screen inversion flash
    M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_fastest);
    M5.Display.display(x, y, w, h);
    M5.Display.setEpdMode(gfxModeFor(_currentMode));
}

void EPDDriver::flushRectGrayscale(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!clipToPanel(x, y, w, h)) return;

    M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_text);
    _partialRefreshCount++; // a real 4-level pass does need the periodic anti-ghosting clean-up
    if (_antiGhostThreshold > 0 && _partialRefreshCount >= _antiGhostThreshold) {
        fullClear(); // restores the driver mode itself
        return;
    }
    M5.Display.display(x, y, w, h);
    M5.Display.setEpdMode(gfxModeFor(_currentMode));
}

void EPDDriver::fullClear() {
    ESP_LOGI(TAG, "Executing Anti-Ghosting Full Clear cycle");
    auto savedMode = _currentMode;
    applyEpdMode(m5gfx::epd_mode_t::epd_quality);
    M5.Display.display();
    M5.Display.waitDisplay();
    _partialRefreshCount = 0;
    setRefreshMode(savedMode);
}

void EPDDriver::resetPartialCounter() {
    _partialRefreshCount = 0;
}

} // namespace MonoMesh
