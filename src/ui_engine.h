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

// One entry of the function drawer (swipe up from the bottom bar). The catalogue is append-only:
// a new function is a new ViewBase in init() plus one entry here, and it shows up in the drawer
// without touching the 4 buttons of the nav bar (which only carries the last-used ones).
struct AppEntry {
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

    // Function Drawer (swipe up from the bottom bar): catalogue of every function, while the nav
    // bar keeps showing the last four selected from here.
    void openFuncDrawer();
    void closeFuncDrawer();
    bool isFuncDrawerOpen() const { return _funcDrawerOpen; }

    // Fast partial refresh helper
    void requestPartialRefresh(int32_t x, int32_t y, int32_t w, int32_t h);
    void requestFullRefresh();

private:
    UIEngine() = default;
    ~UIEngine() = default;

    int _currentViewIndex = 0;
    std::vector<ViewBase*> _views;

    // Navigation Bar State. The bar always has 4 buttons: _navHistory maps each position to a
    // function, most recently selected first (only a pick from the drawer reorders it).
    static constexpr size_t NUM_NAV_ITEMS = 4;

    // Function catalogue: order of the drawer cells (fixed), never reordered by usage. Names are
    // English; the ones that do not fit a 222 px cell get shortened.
    static constexpr size_t NUM_APPS = 5;
    AppEntry _apps[NUM_APPS] = {
        { "CHAT", 0 },
        { "NODES", 1 },
        { "MAP", 2 },
        { "SETTINGS", 3 },
        { "ALARM", 4 }
    };
    int _navHistory[NUM_NAV_ITEMS] = { 0, 1, 2, 3 };

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

    // Function Drawer Overlay State
    bool _funcDrawerOpen = false;

    // Alarm popup: shown while an alarm rings in the normal UI (in standby/clock the alarm uses the
    // clock face + banner instead). It is the topmost overlay and owns every touch while it is up.
    bool _alarmPopupOpen = false;

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
    void drawFuncDrawer(M5GFX& gfx);
    void drawAlarmPopup(M5GFX& gfx);
    bool handleKeyboardTouch(const TouchEvent& ev);
    bool handleNavBarTouch(const TouchEvent& ev);
    bool handleQuickMenuTouch(const TouchEvent& ev);
    bool handleFuncDrawerTouch(const TouchEvent& ev);
    bool handleAlarmPopupTouch(const TouchEvent& ev);

    // Drawer helpers: moves a function to the front of the bar order (returns true when it really
    // changed) and persists it (SD only).
    bool pushHistory(int viewIndex);
    void selectApp(size_t appIndex);
    void loadNavHistory();
    void saveNavHistory() const;

    void drawPowerIcon(M5GFX& gfx, int cx, int cy, int radius);
    void drawStandbyIcon(M5GFX& gfx, int cx, int cy, int radius);
    void drawChargingBolt(M5GFX& gfx, int cx, int cy);
    void drawRadioActivityIcon(M5GFX& gfx, int cx, int cy);
    void drawLedIcon(M5GFX& gfx, int cx, int cy, bool active);
    void drawSpeakerIcon(M5GFX& gfx, int cx, int cy, bool muted);
};

} // namespace MonoMesh
