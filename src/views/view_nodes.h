#pragma once

#include "view_base.h"
#include <cstdint>

namespace MonoMesh {

class ViewNodes : public ViewBase {
public:
    ViewNodes() = default;
    ~ViewNodes() override = default;

    void onEnter() override;
    void draw(M5GFX& gfx) override;
    bool handleTouch(const TouchEvent& ev) override;
    void update() override;
    bool scrollPage(int8_t direction) override;

private:
    int _currentPage = 1;
    uint32_t _selectedNodeNum = 0;
    bool _modalOpen = false;
    bool _tracerouteOpen = false;
    uint32_t _tracerouteStarted = 0;
    uint32_t _tracerouteLastSeen = 0;

    static void drawPadlock(M5GFX& gfx, int x, int y, bool locked);
    void drawTraceroutePanel(M5GFX& gfx);
};

} // namespace MonoMesh
