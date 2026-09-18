#pragma once

#include <Arduino.h>
#include <M5Unified.h>

namespace MonoMesh {

enum class TouchEventType {
    None,
    Down,
    Move,
    Up,
    Click,
    SwipeLeft,
    SwipeRight,
    SwipeDown,
    SwipeUp
};

struct TouchEvent {
    TouchEventType type = TouchEventType::None;
    int16_t x = 0;
    int16_t y = 0;
    int16_t startX = 0;
    int16_t startY = 0;
    uint32_t durationMs = 0;
};

// Phone-style edge swipe ("go back"): the gesture must start within this many pixels of the left
// border, so a normal horizontal drag in the middle of the screen does not navigate away.
constexpr int TOUCH_EDGE_PX = 48;
inline bool isEdgeSwipeRight(const TouchEvent& ev) {
    return ev.type == TouchEventType::SwipeRight && ev.startX <= TOUCH_EDGE_PX;
}

class TouchManager {
public:
    static TouchManager& getInstance() {
        static TouchManager instance;
        return instance;
    }

    bool init();
    void update();

    bool hasEvent() const { return _currentEvent.type != TouchEventType::None; }
    TouchEvent popEvent();
    const TouchEvent& peekEvent() const { return _currentEvent; }

    bool isPressed() const { return _isPressed; }
    void getCoordinates(int16_t& x, int16_t& y) const { x = _curX; y = _curY; }

private:
    TouchManager() = default;
    ~TouchManager() = default;

    bool _isPressed = false;
    int16_t _curX = 0;
    int16_t _curY = 0;
    int16_t _downX = 0;
    int16_t _downY = 0;
    uint32_t _downTime = 0;
    uint32_t _lastM5PollMs = 0;   // keep-alive cadence for M5.update() when the panel is idle
    uint32_t _lastIntLowMs = 0;   // last time TOUCH_INT was seen asserted

    TouchEvent _currentEvent;
};

} // namespace MonoMesh
