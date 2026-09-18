#include "touch_manager.h"
#include "bsp_papermono.h"
#include <esp_log.h>

static constexpr const char* TAG = "MonoMesh-Touch";

namespace MonoMesh {

bool TouchManager::init() {
    ESP_LOGI(TAG, "Touch manager initialized (using M5.Touch.getTouchPointRaw)");
    return true;
}

void TouchManager::update() {
    // M5.update() is the most expensive I2C operation in the main loop (it reads the FT6336 touch
    // controller and the PM1 on the shared 100/400 kHz bus). Poll it immediately while the panel
    // reports activity (TOUCH_INT low, also during a gesture) and at a keep-alive cadence otherwise,
    // which is enough for BtnA/BtnB and the PM1 power-button latch.
    constexpr uint32_t IDLE_POLL_MS = 50;
    uint32_t now = millis();
    const bool panelActive = (digitalRead(Pins::TOUCH_INT) == LOW);
    if (panelActive) _lastIntLowMs = now;
    // M5GFX can leave the FT6336 in "INT polling" mode, where the pin carries no touch information:
    // if it has not pulsed for a while, fall back to polling every loop (the old behaviour) instead
    // of adding latency to the touch response.
    const bool intUsable = (now - _lastIntLowMs) < 3000;
    const uint32_t idlePollMs = intUsable ? IDLE_POLL_MS : 0;
    if (!panelActive && !_isPressed && (now - _lastM5PollMs) < idlePollMs) {
        return; // nothing to refresh: the previous event state stays valid
    }
    _lastM5PollMs = now;

    M5.update();

    _currentEvent.type = TouchEventType::None;

    // Check if finger is currently on screen
    if (!M5.Touch.getCount()) {
        if (_isPressed) {
            _isPressed = false;
            int16_t dx = _curX - _downX;
            int16_t dy = _curY - _downY;
            uint32_t dur = millis() - _downTime;

            if (abs(dx) > 70 && abs(dy) < 60 && dur < 600) {
                _currentEvent.type = (dx > 0) ? TouchEventType::SwipeRight : TouchEventType::SwipeLeft;
            } else if (dy > 60 && abs(dx) < 60 && dur < 600) {
                _currentEvent.type = TouchEventType::SwipeDown;
            } else if (dy < -60 && abs(dx) < 60 && dur < 600) {
                _currentEvent.type = TouchEventType::SwipeUp;
            } else if (abs(dx) < 50 && abs(dy) < 50 && dur < 1000) {
                _currentEvent.type = TouchEventType::Click;
            } else {
                _currentEvent.type = TouchEventType::Up;
            }

            _currentEvent.x = _curX;
            _currentEvent.y = _curY;
            _currentEvent.startX = _downX;
            _currentEvent.startY = _downY;
            _currentEvent.durationMs = dur;

            ESP_LOGD(TAG, "Touch released at (%d, %d), type=%d, dur=%ums",
                     _curX, _curY, static_cast<int>(_currentEvent.type), (unsigned)dur);
        }
        return;
    }

    // Read physical coordinates from FT6336G
    const auto& raw = M5.Touch.getTouchPointRaw();
    int16_t rx = raw.x;
    int16_t ry = raw.y;

    // Clamp within 480x800 portrait bounds
    if (rx < 0) rx = 0;
    if (rx > 479) rx = 479;
    if (ry < 0) ry = 0;
    if (ry > 799) ry = 799;

    _curX = rx;
    _curY = ry;

    if (!_isPressed) {
        _isPressed = true;
        _downX = rx;
        _downY = ry;
        _downTime = millis();

        _currentEvent.type = TouchEventType::Down;
        _currentEvent.x = rx;
        _currentEvent.y = ry;
        _currentEvent.startX = rx;
        _currentEvent.startY = ry;
        _currentEvent.durationMs = 0;

        ESP_LOGD(TAG, "Touch down at (%d, %d)", rx, ry);
        return;
    }

    if (abs(rx - _downX) > 15 || abs(ry - _downY) > 15) {
        _currentEvent.type = TouchEventType::Move;
        _currentEvent.x = rx;
        _currentEvent.y = ry;
        _currentEvent.startX = _downX;
        _currentEvent.startY = _downY;
        _currentEvent.durationMs = millis() - _downTime;
    }
}

TouchEvent TouchManager::popEvent() {
    TouchEvent ev = _currentEvent;
    _currentEvent.type = TouchEventType::None;
    return ev;
}

} // namespace MonoMesh
