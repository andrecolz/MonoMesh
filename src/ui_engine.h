#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <vector>
#include <string>
#include <functional>
#include "views/view_base.h"
#include "touch_manager.h"
#include "epd_driver.h"

namespace MonoMesh {

// Minimal cog used by the nav bar and by the settings header. Declared here so both the UI engine
// and the settings widgets draw the exact same glyph.
void drawGearIcon(M5GFX& gfx, int cx, int cy, uint16_t color, uint16_t bg, int radius = 15);

struct CarouselItem {
    const char* label;
    int viewIndex;
};

class UIEngine {
public:
    static UIEngine& getInstance() {
        static UIEngine instance;
        return instance;
    }

    bool init();
    void update();
    void render();

    // View Navigation
    void setView(int viewIndex);
    int getCurrentViewIndex() const { return _currentViewIndex; }
    // Pagination from the physical buttons: direction -1 = page up, +1 = page down. Returns false
    // when an overlay (keyboard/quick menu) owns the screen or the view has no pages.
    bool scrollCurrentView(int8_t direction);

    // Top Status Bar
    void requestStatusBarRedraw();

    // Virtual Keyboard
    void openKeyboard(const std::string& initialText, std::function<void(const std::string&)> onSend);
    void closeKeyboard();
    bool isKeyboardOpen() const { return _keyboardOpen; }

    // Quick Settings Menu
    void openQuickMenu();
    void closeQuickMenu();
    bool isQuickMenuOpen() const { return _quickMenuOpen; }

    // Fast partial refresh helper
    void requestPartialRefresh(int32_t x, int32_t y, int32_t w, int32_t h);
    void requestFullRefresh();

private:
    UIEngine() = default;
    ~UIEngine() = default;

    int _currentViewIndex = 0;
    std::vector<ViewBase*> _views;

    // Navigation Bar State
    static constexpr size_t NUM_NAV_ITEMS = 4;
    CarouselItem _navItems[NUM_NAV_ITEMS] = {
        { "CHAT", 0 },
        { "NODES", 1 },
        { "MAP", 2 },
        { "SET", 3 }
    };

    // Status Bar & Redraw tracking
    uint32_t _lastStatusUpdate = 0;
    bool _lastChargingState = false;
    bool _lastRxActive = false;
    bool _lastTxActive = false;
    bool _needsFullRedraw = true;
    bool _needsStatusRedraw = true;
    bool _needsNavRedraw = true;

    // Quick Menu Overlay State
    bool _quickMenuOpen = false;

    // Virtual Keyboard State
    enum class KeyShift : uint8_t { Off, Once, Lock };
    bool _keyboardOpen = false;
    std::string _keyboardBuffer;
    KeyShift _keyShift = KeyShift::Off;
    std::function<void(const std::string&)> _onKeyboardSend;

    void drawStatusBar(M5GFX& gfx);
    void drawNavBar(M5GFX& gfx);
    void drawKeyboard(M5GFX& gfx);
    void drawQuickMenu(M5GFX& gfx);
    bool handleKeyboardTouch(const TouchEvent& ev);
    bool handleNavBarTouch(const TouchEvent& ev);
    bool handleQuickMenuTouch(const TouchEvent& ev);

    void drawPowerIcon(M5GFX& gfx, int cx, int cy, int radius);
    void drawStandbyIcon(M5GFX& gfx, int cx, int cy, int radius);
    void drawChargingBolt(M5GFX& gfx, int cx, int cy);
    void drawRadioActivityIcon(M5GFX& gfx, int cx, int cy);
    void drawLedIcon(M5GFX& gfx, int cx, int cy, bool active);
    void drawSpeakerIcon(M5GFX& gfx, int cx, int cy, bool muted);
};

} // namespace MonoMesh
