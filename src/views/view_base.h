#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include "../touch_manager.h"

namespace MonoMesh {

// Shared background for the view header bars: 1-bit dot screen (black 1 px dots on white, 4 px
// diagonal pitch). Pure black/white is what the panel renders identically on the first frame after
// boot and after every 1-bit partial update: a mid-gray fill is pushed to an endpoint by those
// updates (and can show a different texture on the very first pass), so the bar looked glitched
// until a manual full refresh.
inline void headerBackground(M5GFX& gfx, int y, int h) {
    gfx.fillRect(0, y, 480, h, TFT_WHITE);
    for (int row = 0; row < h; ++row) {
        for (int x = (4 - (row & 3)) & 3; x < 480; x += 4) {
            gfx.drawPixel(x, y + row, TFT_BLACK);
        }
    }
}

class ViewBase {
public:
    virtual ~ViewBase() = default;
    virtual void onEnter() {}
    // Called when the view is about to be replaced by another one: lets a view flush pending work
    // (e.g. the settings debounced save) before its screen disappears.
    virtual void onExit() {}
    virtual void draw(M5GFX& gfx) = 0;
    virtual bool handleTouch(const TouchEvent& ev) = 0;
    virtual void update() {}
    // Pagination from the physical buttons: direction is -1 (page up / toward the start) or +1
    // (page down / toward the end). Views without pages simply return false.
    virtual bool scrollPage(int8_t direction) { (void)direction; return false; }
    // Tapping the tab of the view that is already on screen goes back to its root page (the chat
    // conversation list, the settings category list). Returns true when something changed.
    virtual bool resetToRoot() { return false; }
};

} // namespace MonoMesh
