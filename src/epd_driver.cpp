#include "epd_driver.h"
#include <esp_log.h>

static constexpr const char* TAG = "MonoMesh-EPD";

namespace MonoMesh {

bool EPDDriver::init() {
    ESP_LOGI(TAG, "Initializing SSD1677 Display Controller...");
    M5.Display.setRotation(0);
    M5.Display.setAutoDisplay(false);

    setRefreshMode(RefreshMode::Grayscale);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.display();
    M5.Display.waitDisplay();

    ESP_LOGI(TAG, "EPD initialized: %dx%d", M5.Display.width(), M5.Display.height());
    return true;
}

void EPDDriver::applyEpdMode(m5gfx::epd_mode_t mode) {
    M5.Display.setEpdMode(mode);
}

void EPDDriver::setRefreshMode(RefreshMode mode) {
    _currentMode = mode;
    switch (mode) {
        case RefreshMode::FastPartial:
            applyEpdMode(m5gfx::epd_mode_t::epd_fastest);
            break;
        case RefreshMode::Grayscale:
            applyEpdMode(m5gfx::epd_mode_t::epd_text);
            break;
        case RefreshMode::FullClear:
            applyEpdMode(m5gfx::epd_mode_t::epd_quality);
            break;
    }
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

    M5.Display.display();
}

void EPDDriver::flushRect(int32_t x, int32_t y, int32_t w, int32_t h, bool countPartial) {
    // Clip bounding box to screen dimensions
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > M5.Display.width())  w = M5.Display.width() - x;
    if (y + h > M5.Display.height()) h = M5.Display.height() - y;
    if (w <= 0 || h <= 0) return;

    if (_currentMode == RefreshMode::FastPartial) {
        if (countPartial) {
            _partialRefreshCount++;
            if (_antiGhostThreshold > 0 && _partialRefreshCount >= _antiGhostThreshold) {
                fullClear();
                return;
            }
        }
    }

    // Always enforce 1-bit fast LUT for rectangular partial updates to prevent screen inversion flash
    auto prevMode = M5.Display.getEpdMode();
    M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_fastest);
    M5.Display.display(x, y, w, h);
    M5.Display.setEpdMode(prevMode);
}

void EPDDriver::flushRectGrayscale(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > M5.Display.width())  w = M5.Display.width() - x;
    if (y + h > M5.Display.height()) h = M5.Display.height() - y;
    if (w <= 0 || h <= 0) return;

    auto prevMode = M5.Display.getEpdMode();
    M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_text);
    _partialRefreshCount++; // a real 4-level pass does need the periodic anti-ghosting clean-up
    if (_antiGhostThreshold > 0 && _partialRefreshCount >= _antiGhostThreshold) {
        M5.Display.setEpdMode(prevMode);
        fullClear();
        return;
    }
    M5.Display.display(x, y, w, h);
    M5.Display.setEpdMode(prevMode);
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
