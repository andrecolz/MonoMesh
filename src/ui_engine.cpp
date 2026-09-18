#include "ui_engine.h"
#include "bsp_papermono.h"
#include "mesh_service.h"
#include "views/view_chat.h"
#include "views/view_nodes.h"
#include "views/view_map.h"
#include "views/view_settings.h"
#include <esp_log.h>
#include <cctype>

static constexpr const char* TAG = "MonoMesh-UI";

namespace MonoMesh {

// Keyboard geometry shared by the draw and touch paths. Only the input box (a 46 px tall strip) is
// pushed to the e-paper panel while typing: flushing the whole 370 px keyboard costs ~8x more SPI
// traffic and panel time (~150-200 ms vs ~25-40 ms per keystroke).
static constexpr int32_t KB_BOX_X = 6;
static constexpr int32_t KB_BOX_Y = 434;
static constexpr int32_t KB_BOX_W = 468;
static constexpr int32_t KB_BOX_H = 46;
// Longest text the radio can carry: with PKI a DM frame fits 222 bytes, so a longer buffer would
// only be truncated (invisibly) at send time. 224 is the hard width of ChatMessage::text anyway.
static constexpr size_t KB_MAX_CHARS = 222;

// Quick menu geometry shared by the draw and touch paths. The panel hangs from the status bar with
// zero side margins: while it is open the bar separator row is covered (so bar + menu read as one
// surface) and only the bottom edge is closed by a 2 px line with rounded corners.
static constexpr int32_t QM_X = 0;
static constexpr int32_t QM_Y = 43;                // covers the status bar separator row
static constexpr int32_t QM_W = 480;
static constexpr int32_t QM_H = 171;               // Y: 43..213
static constexpr int32_t QM_BOTTOM = QM_Y + QM_H;  // exclusive
static constexpr int32_t QM_R = 18;                // bottom corner radius
static constexpr int32_t QM_T = 2;                 // bottom border thickness

// Quick menu button grid: same 14..466 span as the chat/settings cards (452 px wide, exclusive
// right edge 466), two 74 px buttons + one 284 px box per row, 10 px gaps.
static constexpr int32_t QM_COL1_X = 14;
static constexpr int32_t QM_COL2_X = 98;
static constexpr int32_t QM_BOX_X = 182;
static constexpr int32_t QM_BOX_W = 284;
static constexpr int32_t QM_ROW1_Y = 51;
static constexpr int32_t QM_ROW2_Y = 131;
static constexpr int32_t QM_BTN_W = 74;
static constexpr int32_t QM_BTN_H = 70;
static constexpr int32_t QM_STEP_W = 58;           // [-] / [+] buttons inside the boxes
static constexpr int32_t QM_STEP_H = 58;
static constexpr int32_t QM_STEP_X1 = QM_BOX_X + 8;             // [-]
static constexpr int32_t QM_STEP_X2 = QM_BOX_X + QM_BOX_W - 66; // [+]
static constexpr int32_t QM_STEP_Y1 = QM_ROW1_Y + 6;
static constexpr int32_t QM_STEP_Y2 = QM_ROW2_Y + 6;

bool UIEngine::init() {
    ESP_LOGI(TAG, "Initializing UI Engine & Views...");

    _views.push_back(new ViewChat());     // View 0 (CHAT - unified channels & DMs)
    _views.push_back(new ViewNodes());    // View 1 (NODI)
    _views.push_back(new ViewMap());      // View 2 (MAPPA)
    _views.push_back(new ViewSettings()); // View 3 (SET)

    _currentViewIndex = 0;
    _views[_currentViewIndex]->onEnter();

    _needsFullRedraw = true;
    return true;
}

void UIEngine::setView(int viewIndex) {
    if (viewIndex >= 0 && viewIndex < (int)_views.size() && viewIndex != _currentViewIndex) {
        if (_currentViewIndex >= 0 && _currentViewIndex < (int)_views.size()) {
            _views[_currentViewIndex]->onExit();
        }
        _currentViewIndex = viewIndex;
        _views[_currentViewIndex]->onEnter();
        _needsFullRedraw = true;
    }
}

bool UIEngine::scrollCurrentView(int8_t direction) {
    // While the keyboard or the quick menu is on screen the physical buttons must not move the view
    // underneath it: the overlay is what the user is looking at.
    if (_quickMenuOpen || _keyboardOpen) return false;
    if (_currentViewIndex < 0 || _currentViewIndex >= (int)_views.size()) return false;
    return _views[_currentViewIndex]->scrollPage(direction);
}

void UIEngine::requestStatusBarRedraw() {
    _needsStatusRedraw = true;
}

void UIEngine::requestFullRefresh() {
    _needsFullRedraw = true;
}

void UIEngine::requestPartialRefresh(int32_t x, int32_t y, int32_t w, int32_t h) {
    EPDDriver::getInstance().flushRect(x, y, w, h);
}

void UIEngine::openKeyboard(const std::string& initialText, std::function<void(const std::string&)> onSend) {
    _keyboardOpen = true;
    _keyboardBuffer = (initialText.size() > KB_MAX_CHARS) ? initialText.substr(0, KB_MAX_CHARS) : initialText;
    _keyShift = KeyShift::Off;
    _onKeyboardSend = onSend;

    // Fast partial switch for responsive typing
    EPDDriver::getInstance().setRefreshMode(RefreshMode::FastPartial);
    drawKeyboard(M5.Display);
    EPDDriver::getInstance().flushRect(0, 430, 480, 370);
}

void UIEngine::closeKeyboard() {
    if (_keyboardOpen) {
        _keyboardOpen = false;
        EPDDriver::getInstance().setRefreshMode(RefreshMode::Grayscale);
        _needsFullRedraw = true;
    }
}

void UIEngine::openQuickMenu() {
    _quickMenuOpen = true;
    EPDDriver::getInstance().setRefreshMode(RefreshMode::Grayscale);
    _needsFullRedraw = true;
}

void UIEngine::closeQuickMenu() {
    if (_quickMenuOpen) {
        _quickMenuOpen = false;
        EPDDriver::getInstance().setRefreshMode(RefreshMode::Grayscale);
        requestFullRefresh();
    }
}

void UIEngine::update() {
    uint32_t now = millis();

    // Update current active view
    if (_currentViewIndex >= 0 && _currentViewIndex < (int)_views.size()) {
        _views[_currentViewIndex]->update();
    }

    // Refresh status bar once per minute or when unread/activity state changes
    static int lastMinute = -1;
    int hour = 0, minute = 0, second = 0;
    BSP::getInstance().getRtcTime(hour, minute, second);
    if (minute != lastMinute) {
        lastMinute = minute;
        _needsStatusRedraw = true;
    }

    // Immediate detection of battery charging state change
    BatteryState curBat = BSP::getInstance().getBatteryState();
    if (curBat.isCharging != _lastChargingState) {
        _lastChargingState = curBat.isCharging;
        _needsStatusRedraw = true;
    }

    // Radio activity icon: follow TX/RX in near real time (MeshService clears the flags ~2 s after
    // the last activity, so otherwise the icon would only move at the minute change).
    const bool rxActive = MeshService::getInstance().isRxActive();
    const bool txActive = MeshService::getInstance().isTxActive();
    if (rxActive != _lastRxActive || txActive != _lastTxActive) {
        _lastRxActive = rxActive;
        _lastTxActive = txActive;
        _needsStatusRedraw = true;
    }

    // Check touch events
    if (TouchManager::getInstance().hasEvent()) {
        TouchEvent ev = TouchManager::getInstance().popEvent();

        // 1. If Quick Menu is open, it consumes touch events
        if (_quickMenuOpen) {
            if (handleQuickMenuTouch(ev)) {
                return;
            }
        }

        // 2. Top bar click or swipe down *starting inside the top bar* opens the Quick Menu (works
        // even if the keyboard is open). A swipe that starts lower down belongs to the view: on the
        // map it used to steal the gesture needed to pan the map.
        if (ev.type == TouchEventType::Click && ev.y >= 0 && ev.y < 44) {
            openQuickMenu();
            return;
        }
        if (ev.type == TouchEventType::SwipeDown && ev.startY >= 0 && ev.startY < 44) {
            openQuickMenu();
            return;
        }

        // 3. If Keyboard is open, it handles touch
        if (_keyboardOpen) {
            if (handleKeyboardTouch(ev)) {
                return;
            }
        } else {
            // 4. Bottom Nav Bar handles touch (Y: 730..800) only when keyboard is closed
            if (!_quickMenuOpen && ev.y >= 730 && ev.y <= 800) {
                if (handleNavBarTouch(ev)) {
                    return;
                }
            }
        }

        // 5. Current active view handles touch
        int viewBottomLimit = _keyboardOpen ? 430 : 730;
        if (!_quickMenuOpen && ev.y >= 44 && ev.y < viewBottomLimit) {
            if (_views[_currentViewIndex]->handleTouch(ev)) {
                return;
            }
        }
    }
}

void UIEngine::render() {
    // Any low-power mode owns the panel (standby logo or clock face): the UI must not draw over it.
    if (BSP::getInstance().isLowPower()) return;

    auto& gfx = M5.Display;

    if (_needsFullRedraw) {
        _needsFullRedraw = false;
        _needsStatusRedraw = false;
        _needsNavRedraw = false;

        EPDDriver::getInstance().setRefreshMode(RefreshMode::Grayscale);
        gfx.fillScreen(TFT_WHITE);

        drawStatusBar(gfx);

        if (_currentViewIndex >= 0 && _currentViewIndex < (int)_views.size()) {
            _views[_currentViewIndex]->draw(gfx);
        }

        if (_keyboardOpen) {
            drawKeyboard(gfx);
        } else {
            drawNavBar(gfx);
        }

        if (_quickMenuOpen) {
            drawQuickMenu(gfx);
        }

        EPDDriver::getInstance().flush();
        return;
    }

    if (_needsStatusRedraw && !_keyboardOpen && !_quickMenuOpen) {
        _needsStatusRedraw = false;
        drawStatusBar(gfx);
        EPDDriver::getInstance().flushRect(0, 0, 480, 44);
    }

    if (_needsNavRedraw && !_keyboardOpen && !_quickMenuOpen) {
        _needsNavRedraw = false;
        drawNavBar(gfx);
        EPDDriver::getInstance().flushRect(0, 730, 480, 70);
    }
}

// One arrow of the RX/TX indicator: pointing up (TX) or down (RX). Drawn as a single closed
// outline (head and shaft are one piece: the barbs close onto the shaft walls, no detached bar and
// chevron), and filled solid while that direction is active.
static void drawRadioArrow(M5GFX& gfx, int cx, int cy, bool up, bool active) {
    const int barbHalf = 6;   // head: 13 px wide
    const int shaftHalf = 2;  // shaft: 5 px wide
    const int yTop = up ? (cy - 10) : (cy + 10);
    const int yBarb = up ? (cy - 1) : (cy + 1);
    const int yBottom = up ? (cy + 10) : (cy - 10);
    const int yShaft = (yBarb < yBottom) ? yBarb : yBottom;
    const int shaftH = ((yBottom > yBarb) ? (yBottom - yBarb) : (yBarb - yBottom)) + 1;

    if (active) {
        gfx.fillRect(cx - shaftHalf, yShaft, 2 * shaftHalf + 1, shaftH, TFT_BLACK);
        gfx.fillTriangle(cx - barbHalf, yBarb, cx + barbHalf, yBarb, cx, yTop, TFT_BLACK);
        gfx.fillCircle(cx, yBottom, shaftHalf, TFT_BLACK); // rounded end like the reference glyph
        return;
    }

    // Outline: head slopes, barb under-lines that join the shaft walls, then the shaft itself.
    gfx.drawLine(cx - barbHalf, yBarb, cx, yTop, TFT_BLACK);
    gfx.drawLine(cx + barbHalf, yBarb, cx, yTop, TFT_BLACK);
    gfx.drawFastHLine(cx - barbHalf, yBarb, barbHalf - shaftHalf + 1, TFT_BLACK);
    gfx.drawFastHLine(cx + shaftHalf, yBarb, barbHalf - shaftHalf + 1, TFT_BLACK);
    gfx.drawFastVLine(cx - shaftHalf, yShaft, shaftH, TFT_BLACK);
    gfx.drawFastVLine(cx + shaftHalf, yShaft, shaftH, TFT_BLACK);
    gfx.drawFastHLine(cx - shaftHalf, yBottom, 2 * shaftHalf + 1, TFT_BLACK);
}

// Radio activity: down arrow (RX) on the left, up arrow (TX) on the right, to the left of the
// battery and with a gap from the charging bolt (drawn at batX - 14).
void UIEngine::drawRadioActivityIcon(M5GFX& gfx, int cx, int cy) {
    drawRadioArrow(gfx, cx - 9, cy, false, MeshService::getInstance().isRxActive());
    drawRadioArrow(gfx, cx + 9, cy, true, MeshService::getInstance().isTxActive());
}

void UIEngine::drawStatusBar(M5GFX& gfx) {
    // Height: 44 px (Y: 0..43) - unchanged.
    gfx.fillRect(0, 0, 480, 43, TFT_WHITE);
    gfx.drawFastHLine(0, 43, 480, TFT_BLACK);

    // 1. RTC Time (HH:MM) - size 2.5, shifted down (the GLCD glyphs only occupy rows 0..6 of the
    // 8-row cell, so a "middle" datum would leave them visibly high in the 44 px bar) and aligned
    // with the header text below.
    int hour = 0, minute = 0, second = 0;
    BSP::getInstance().getRtcTime(hour, minute, second);
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hour, minute);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(2.5f);
    gfx.drawString(timeStr, 18, 25);

    // 2. LoRa RX/TX activity icon, next to the battery (replaces the "CH: N%" metric and the LORA box)
    drawRadioActivityIcon(gfx, 366, 23);

    // 3. Notification Badge
    uint32_t unread = MeshService::getInstance().getUnreadCount();
    if (unread > 0) {
        int badgeX = 265;
        gfx.fillRoundRect(badgeX, 10, 42, 26, 4, TFT_BLACK);
        gfx.setTextColor(TFT_WHITE, TFT_BLACK);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        char bStr[8];
        snprintf(bStr, sizeof(bStr), "[%u]", (unsigned)unread);
        gfx.drawString(bStr, badgeX + 21, 24);
    }

    // 4. Battery State (Only icon with discrete ticks, NO percentage). A few px larger than before
    // and nudged down like the clock, so it reads centred in the bar; the bolt stays on its left.
    BatteryState bat = BSP::getInstance().getBatteryState();
    // Critically low cell: the icon goes inverted, so the warning shows up on any screen without
    // stealing space from the views. The cutoff itself lives in main.cpp.
    const bool lowBattery = !bat.isCharging && bat.voltageMv > 0 &&
                            bat.voltageMv <= BSP::LOW_BATTERY_WARN_MV;
    int batX = 410;
    int batY = 10;
    int batW = 58;
    int batH = 26;

    if (lowBattery) {
        gfx.fillRoundRect(batX, batY, batW, batH, 3, TFT_BLACK);
        gfx.fillRect(batX + batW, batY + (batH - 12) / 2, 4, 12, TFT_BLACK); // battery terminal
    } else {
        gfx.drawRoundRect(batX, batY, batW, batH, 3, TFT_BLACK);
        gfx.fillRect(batX + batW, batY + (batH - 12) / 2, 4, 12, TFT_BLACK); // battery terminal
    }

    // Draw 4 discrete battery level bars (ticks)
    int numBars = 0;
    if (bat.percentage >= 80) numBars = 4;
    else if (bat.percentage >= 55) numBars = 3;
    else if (bat.percentage >= 30) numBars = 2;
    else if (bat.percentage >= 10) numBars = 1;

    int barW = 10;
    int barH = 18;
    int barGap = 2;
    int barStartX = batX + 6; // centred inside the 2 px border (54 - 46) / 2
    int barStartY = batY + 4;

    for (int b = 0; b < numBars; ++b) {
        gfx.fillRect(barStartX + b * (barW + barGap), barStartY, barW, barH,
                     lowBattery ? TFT_WHITE : TFT_BLACK);
    }

    // Charging indicator (sleek, symmetrical lightning bolt placed just left of the battery icon)
    if (bat.isCharging) {
        drawChargingBolt(gfx, batX - 14, batY + batH / 2);
    }
}

// Minimal filled cog (settings): a solid body, eight rounded teeth and a round hole, like the
// classic Material gear. Drawn with primitives only, so it stays readable at the nav-bar size and
// can be inverted (fg/bg) on the active tab.
void drawGearIcon(M5GFX& gfx, int cx, int cy, uint16_t color, uint16_t bg, int radius) {
    const int bodyR = (radius * 74) / 100;
    const int holeR = (radius * 34) / 100;
    const int halfW = (radius * 29) / 100; // tooth half-width (and tip rounding radius)
    for (int k = 0; k < 8; ++k) {
        const float a = k * 0.7853982f; // 45 degrees
        const float ca = cosf(a), sa = sinf(a);
        const int baseR = bodyR - 1;
        const int tipR = radius - halfW;
        for (int off = -halfW; off <= halfW; ++off) {
            const int bx = cx + (int)lroundf(ca * baseR) - (int)lroundf(sa * off);
            const int by = cy + (int)lroundf(sa * baseR) + (int)lroundf(ca * off);
            const int tx = cx + (int)lroundf(ca * tipR) - (int)lroundf(sa * off);
            const int ty = cy + (int)lroundf(sa * tipR) + (int)lroundf(ca * off);
            gfx.drawLine(bx, by, tx, ty, color);
        }
        // Rounded tip
        gfx.fillCircle(cx + (int)lroundf(ca * tipR), cy + (int)lroundf(sa * tipR), halfW, color);
    }
    gfx.fillCircle(cx, cy, bodyR, color);
    gfx.fillCircle(cx, cy, holeR, bg);
}

// fg = icon colour, bg = colour of the button behind it (used for the "holes" so the icon can also
// be drawn inverted on the active tab).
static void drawNavIcon(M5GFX& gfx, int index, int cx, int cy, uint16_t color, uint16_t bg) {
    switch (index) {
        case 0: { // CHAT: Speech bubble
            gfx.drawRoundRect(cx - 16, cy - 12, 32, 22, 5, color);
            gfx.drawRoundRect(cx - 15, cy - 11, 30, 20, 4, color);
            gfx.fillTriangle(cx - 10, cy + 9, cx - 4, cy + 9, cx - 12, cy + 16, color);
            gfx.fillRoundRect(cx - 10, cy - 6, 20, 3, 1, color);
            gfx.fillRoundRect(cx - 10, cy, 14, 3, 1, color);
            break;
        }
        case 1: { // NODI: Mesh Network of 3 connected nodes
            gfx.drawLine(cx, cy - 10, cx - 12, cy + 9, color);
            gfx.drawLine(cx + 1, cy - 10, cx - 11, cy + 9, color);
            gfx.drawLine(cx, cy - 10, cx + 12, cy + 9, color);
            gfx.drawLine(cx - 1, cy - 10, cx + 11, cy + 9, color);
            gfx.drawLine(cx - 12, cy + 9, cx + 12, cy + 9, color);
            gfx.drawLine(cx - 12, cy + 10, cx + 12, cy + 10, color);
            gfx.fillCircle(cx, cy - 10, 5, color);
            gfx.fillCircle(cx - 12, cy + 9, 5, color);
            gfx.fillCircle(cx + 12, cy + 9, 5, color);
            break;
        }
        case 2: { // MAPPA: Compass with dial & needle
            gfx.drawCircle(cx, cy, 14, color);
            gfx.drawCircle(cx, cy, 13, color);
            gfx.drawFastVLine(cx, cy - 14, 3, color);
            gfx.drawFastVLine(cx, cy + 12, 3, color);
            gfx.drawFastHLine(cx - 14, cy, 3, color);
            gfx.drawFastHLine(cx + 12, cy, 3, color);
            gfx.fillTriangle(cx, cy - 10, cx - 4, cy + 1, cx + 4, cy + 1, color);
            gfx.drawTriangle(cx, cy + 10, cx - 4, cy - 1, cx + 4, cy - 1, color);
            gfx.fillCircle(cx, cy, 2, color);
            break;
        }
        case 3: { // SET: minimal cog / gear
            drawGearIcon(gfx, cx, cy, color, bg);
            break;
        }
    }
}

void UIEngine::drawNavBar(M5GFX& gfx) {
    // Height: 70 px (Y: 730..799)
    gfx.fillRect(0, 730, 480, 70, TFT_WHITE);

    // 4 Direct App Buttons across 480 px width:
    // startX = 10, itemW = 110, gap = 6 -> 10 + 4*110 + 3*6 = 468 px
    int startX = 10;
    int itemW = 110;
    int gap = 6;

    for (size_t i = 0; i < NUM_NAV_ITEMS; ++i) {
        const auto& item = _navItems[i];
        bool isActive = (item.viewIndex == _currentViewIndex);

        int bx = startX + i * (itemW + gap);
        // The tab we are on is drawn inverted (black background, white icon) so the current view is
        // obvious at a glance.
        const uint16_t bg = isActive ? TFT_BLACK : TFT_WHITE;
        const uint16_t fg = isActive ? TFT_WHITE : TFT_BLACK;
        gfx.drawRoundRect(bx, 730, itemW, 62, 5, TFT_BLACK);
        gfx.fillRoundRect(bx + 1, 731, itemW - 2, 60, 4, bg);

        int cx = bx + itemW / 2;
        int cy = 761;
        drawNavIcon(gfx, (int)i, cx, cy, fg, bg);
    }
}

bool UIEngine::handleNavBarTouch(const TouchEvent& ev) {
    if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return false;

    int startX = 10;
    int itemW = 110;
    int gap = 6;

    for (size_t i = 0; i < NUM_NAV_ITEMS; ++i) {
        int bx = startX + i * (itemW + gap);
        if (ev.x >= bx && ev.x <= bx + itemW) {
            int target = _navItems[i].viewIndex;
            if (target == _currentViewIndex) {
                // Tapping the tab of the view already on screen returns to its root page (chat
                // conversation list, settings category list).
                _views[_currentViewIndex]->resetToRoot();
            } else {
                setView(target);
            }
            _needsNavRedraw = true;
            return true;
        }
    }
    return false;
}

void UIEngine::drawKeyboard(M5GFX& gfx) {
    // Large Keyboard occupies Y: 430..799 (H: 370 px, covers nav bar)
    gfx.fillRect(0, 430, 480, 370, TFT_WHITE);

    // Input preview box (Y: 434..480, H: 46 px)
    gfx.drawRoundRect(KB_BOX_X, KB_BOX_Y, KB_BOX_W, KB_BOX_H, 5, TFT_BLACK);
    gfx.fillRoundRect(KB_BOX_X + 2, KB_BOX_Y + 2, KB_BOX_W - 4, KB_BOX_H - 4, 4, TFT_LIGHTGRAY);
    gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(2);
    // Scroll the field: show the tail that fits, so typing at the limit keeps the caret visible
    // instead of running off the box (the whole keyboard is refreshed with a partial update).
    const int maxTextPx = 448;
    const size_t maxChars = 40; // more than can ever fit: keeps the measuring loop bounded
    std::string shown;
    const int firstIdx = (int)((_keyboardBuffer.size() > maxChars) ? (_keyboardBuffer.size() - maxChars) : 0);
    for (int i = (int)_keyboardBuffer.size() - 1; i >= firstIdx; --i) {
        std::string candidate = _keyboardBuffer.substr((size_t)i) + "_";
        if (gfx.textWidth(candidate.c_str()) > maxTextPx && !shown.empty()) break;
        shown = candidate;
        if (gfx.textWidth(shown.c_str()) > maxTextPx) break;
    }
    if (shown.empty()) shown = "_";
    std::string preview = shown;
    gfx.drawString(preview.c_str(), 16, 457);

    int kw = 42;
    int kh = 54;
    int kgap = 5;

    // Row 0: Numbers, or symbols while shift/caps is active (Y: 488..542)
    const char* row0[] = { "1","2","3","4","5","6","7","8","9","0" };
    const char* sym0[] = { ",",".",";",":","?","!","\"","/","-","'" };
    const bool shifted = (_keyShift != KeyShift::Off);
    // Keycaps follow the shift state (lowercase when off, uppercase when shift/caps is active).
    // The typed character matches what the keycap shows.
    char caseBuf[2] = {0, 0};
    auto drawLetter = [&](char c, int x, int y) {
        caseBuf[0] = shifted ? c : (char)tolower((unsigned char)c);
        gfx.drawString(caseBuf, x, y);
    };
    for (int i = 0; i < 10; ++i) {
        int kx = 7 + i * (kw + kgap);
        gfx.drawRoundRect(kx, 488, kw, kh, 5, TFT_BLACK);
        gfx.fillRoundRect(kx + 1, 489, kw - 2, kh - 2, 4, TFT_WHITE);
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.drawString(shifted ? sym0[i] : row0[i], kx + kw / 2, 488 + kh / 2);
    }

    // Row 1: Q W E R T Y U I O P (Y: 548..602)
    const char* row1 = "QWERTYUIOP";
    for (int i = 0; i < 10; ++i) {
        int kx = 7 + i * (kw + kgap);
        gfx.drawRoundRect(kx, 548, kw, kh, 5, TFT_BLACK);
        gfx.fillRoundRect(kx + 1, 549, kw - 2, kh - 2, 4, TFT_WHITE);
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        drawLetter(row1[i], kx + kw / 2, 548 + kh / 2);
    }

    // Row 2: A S D F G H J K L (Y: 608..662)
    const char* row2 = "ASDFGHJKL";
    int kw2 = 46;
    int r2StartX = 12;
    for (int i = 0; i < 9; ++i) {
        int kx = r2StartX + i * (kw2 + kgap);
        gfx.drawRoundRect(kx, 608, kw2, kh, 5, TFT_BLACK);
        gfx.fillRoundRect(kx + 1, 609, kw2 - 2, kh - 2, 4, TFT_WHITE);
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        drawLetter(row2[i], kx + kw2 / 2, 608 + kh / 2);
    }

    // Row 3: Z X C V B N M + [<-] (Y: 668..722)
    const char* row3 = "ZXCVBNM";
    int kw3 = 44;
    int r3StartX = 12;
    for (int i = 0; i < 7; ++i) {
        int kx = r3StartX + i * (kw3 + kgap);
        gfx.drawRoundRect(kx, 668, kw3, kh, 5, TFT_BLACK);
        gfx.fillRoundRect(kx + 1, 669, kw3 - 2, kh - 2, 4, TFT_WHITE);
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        drawLetter(row3[i], kx + kw3 / 2, 668 + kh / 2);
    }
    // Backspace [<-] (W: 110 px)
    int bkX = r3StartX + 7 * (kw3 + kgap);
    int bkW = 480 - bkX - 12;
    gfx.drawRoundRect(bkX, 668, bkW, kh, 5, TFT_BLACK);
    gfx.fillRoundRect(bkX + 1, 669, bkW - 2, kh - 2, 4, TFT_LIGHTGRAY);
    gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.drawString("<-", bkX + bkW / 2, 668 + kh / 2);

    // Row 4: [MAIUSC], [ SPAZIO ], [ INVIA ] (Y: 728..786, H: 58 px)
    // Tap cycles shift: off -> one-shot -> caps lock -> off. The close action moved to the usual
    // gestures (swipe right, or tap above the keyboard).
    int r4H = 58;
    const bool lock = (_keyShift == KeyShift::Lock);
    gfx.drawRoundRect(10, 728, 100, r4H, 5, TFT_BLACK);
    gfx.fillRoundRect(11, 729, 98, r4H - 2, 4, lock ? TFT_BLACK : TFT_WHITE);
    {
        const int cx = 60;
        const int cy = 728 + r4H / 2 + 2;
        const uint16_t fg = lock ? TFT_WHITE : TFT_BLACK;
        // Up arrow (hollow when off, filled otherwise) + underline bar
        gfx.fillTriangle(cx - 11, cy - 2, cx + 11, cy - 2, cx, cy - 15, fg);
        if (_keyShift == KeyShift::Off) {
            gfx.fillTriangle(cx - 6, cy - 3, cx + 6, cy - 3, cx, cy - 9, lock ? TFT_BLACK : TFT_WHITE);
        }
        gfx.fillRect(cx - 9, cy + 1, 18, 4, fg);
        if (lock) {
            // Caps lock: a second bar marks the latched state
            gfx.fillRect(cx - 9, cy + 8, 18, 3, fg);
        }
    }

    // Space
    gfx.drawRoundRect(120, 728, 236, r4H, 5, TFT_BLACK);
    gfx.fillRoundRect(121, 729, 234, r4H - 2, 4, TFT_WHITE);
    gfx.drawString("SPACE", 238, 728 + r4H / 2);

    // Send
    gfx.drawRoundRect(366, 728, 104, r4H, 5, TFT_BLACK);
    gfx.fillRoundRect(367, 729, 102, r4H - 2, 4, TFT_LIGHTGRAY);
    gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    gfx.drawString("SEND", 418, 728 + r4H / 2);
}

bool UIEngine::handleKeyboardTouch(const TouchEvent& ev) {
    // 1. Swipe right to close keyboard
    if (ev.type == TouchEventType::SwipeRight) {
        closeKeyboard();
        return true;
    }

    // 2. Click outside the keyboard (Y < 430) closes the keyboard only if QuickMenu is not active
    if (!_quickMenuOpen && (ev.type == TouchEventType::Click || ev.type == TouchEventType::Up)) {
        if (ev.y >= 44 && ev.y < 430) {
            closeKeyboard();
            return true;
        }
    }

    if (ev.type != TouchEventType::Click) return true; // Consume other touches while keyboard open
    if (ev.y < 430 || ev.y > 800) return true;

    int kw = 42;
    int kh = 54;
    int kgap = 5;
    const bool shifted = (_keyShift != KeyShift::Off);

    // One-shot shift is consumed by the next keypress (letters and symbols alike)
    auto typeChar = [&](char c) {
        if (_keyboardBuffer.size() >= KB_MAX_CHARS) {
            ESP_LOGW(TAG, "Keyboard limit reached (%u characters)", (unsigned)KB_MAX_CHARS);
            return;
        }
        _keyboardBuffer += c;
        const bool consumedShift = (_keyShift == KeyShift::Once);
        if (consumedShift) _keyShift = KeyShift::Off;
        drawKeyboard(M5.Display);
        // When the one-shot shift is consumed the keycaps change back to lowercase, so the whole
        // keyboard area has to be pushed - flushing only the input box left stale uppercase caps.
        if (consumedShift) {
            EPDDriver::getInstance().flushRect(0, 430, 480, 370);
        } else {
            EPDDriver::getInstance().flushRect(KB_BOX_X, KB_BOX_Y, KB_BOX_W, KB_BOX_H);
        }
    };

    // Row 0: digits, or symbols while shift/caps is active (Y: 488..542)
    if (ev.y >= 488 && ev.y <= 542) {
        const char* r0 = "1234567890";
        const char* s0 = ",.;:?!\"/-'";
        for (int i = 0; i < 10; ++i) {
            int kx = 7 + i * (kw + kgap);
            if (ev.x >= kx && ev.x <= kx + kw) {
                typeChar(shifted ? s0[i] : r0[i]);
                return true;
            }
        }
    }

    // Row 1: Q W E R T Y U I O P (Y: 548..602)
    if (ev.y >= 548 && ev.y <= 602) {
        const char* r1 = "QWERTYUIOP";
        for (int i = 0; i < 10; ++i) {
            int kx = 7 + i * (kw + kgap);
            if (ev.x >= kx && ev.x <= kx + kw) {
                typeChar(shifted ? r1[i] : (char)tolower((unsigned char)r1[i]));
                return true;
            }
        }
    }

    // Row 2: A S D F G H J K L (Y: 608..662)
    if (ev.y >= 608 && ev.y <= 662) {
        const char* r2 = "ASDFGHJKL";
        int kw2 = 46;
        int r2StartX = 12;
        for (int i = 0; i < 9; ++i) {
            int kx = r2StartX + i * (kw2 + kgap);
            if (ev.x >= kx && ev.x <= kx + kw2) {
                typeChar(shifted ? r2[i] : (char)tolower((unsigned char)r2[i]));
                return true;
            }
        }
    }

    // Row 3: Z X C V B N M + Bksp (Y: 668..722)
    if (ev.y >= 668 && ev.y <= 722) {
        const char* r3 = "ZXCVBNM";
        int kw3 = 44;
        int r3StartX = 12;
        for (int i = 0; i < 7; ++i) {
            int kx = r3StartX + i * (kw3 + kgap);
            if (ev.x >= kx && ev.x <= kx + kw3) {
                typeChar(shifted ? r3[i] : (char)tolower((unsigned char)r3[i]));
                return true;
            }
        }
        // Backspace
        int bkX = r3StartX + 7 * (kw3 + kgap);
        if (ev.x >= bkX && ev.x <= 470) {
            if (!_keyboardBuffer.empty()) {
                _keyboardBuffer.pop_back();
                drawKeyboard(M5.Display);
                EPDDriver::getInstance().flushRect(KB_BOX_X, KB_BOX_Y, KB_BOX_W, KB_BOX_H);
            }
            return true;
        }
    }

    // Row 4: Shift, Space, Send (Y: 728..786)
    if (ev.y >= 728 && ev.y <= 790) {
        // Shift / caps: off -> once -> lock -> off
        if (ev.x >= 10 && ev.x <= 110) {
            switch (_keyShift) {
                case KeyShift::Off:  _keyShift = KeyShift::Once; break;
                case KeyShift::Once: _keyShift = KeyShift::Lock; break;
                case KeyShift::Lock: _keyShift = KeyShift::Off;  break;
            }
            drawKeyboard(M5.Display);
            EPDDriver::getInstance().flushRect(0, 430, 480, 370);
            return true;
        }
        // Space
        if (ev.x >= 120 && ev.x <= 356) {
            if (_keyboardBuffer.size() >= KB_MAX_CHARS) {
                ESP_LOGW(TAG, "Keyboard limit reached (%u characters)", (unsigned)KB_MAX_CHARS);
                return true;
            }
            _keyboardBuffer += ' ';
            drawKeyboard(M5.Display);
            EPDDriver::getInstance().flushRect(KB_BOX_X, KB_BOX_Y, KB_BOX_W, KB_BOX_H);
            return true;
        }
        // Send
        if (ev.x >= 366 && ev.x <= 474) {
            std::string text = _keyboardBuffer;
            closeKeyboard();
            if (_onKeyboardSend) {
                _onKeyboardSend(text);
            }
            return true;
        }
    }

    return true;
}

void UIEngine::drawChargingBolt(M5GFX& gfx, int cx, int cy) {
    // Sharp symmetrical black vector lightning bolt
    gfx.fillTriangle(cx + 1, cy - 9, cx - 6, cy + 1, cx + 2, cy + 1, TFT_BLACK);
    gfx.fillTriangle(cx - 1, cy + 9, cx + 6, cy - 1, cx - 2, cy - 1, TFT_BLACK);
    gfx.fillRect(cx - 2, cy - 1, 5, 3, TFT_BLACK);
}

void UIEngine::drawPowerIcon(M5GFX& gfx, int cx, int cy, int radius) {
    // Standard power icon: Circle arc with top vertical notch
    for (int r = radius - 2; r <= radius + 1; ++r) {
        gfx.drawCircle(cx, cy, r, TFT_BLACK);
    }
    // Erase top opening notch
    gfx.fillRect(cx - 8, cy - radius - 3, 16, 12, TFT_WHITE);
    // Vertical center stroke
    gfx.fillRect(cx - 3, cy - radius - 4, 6, radius + 2, TFT_BLACK);
}

void UIEngine::drawStandbyIcon(M5GFX& gfx, int cx, int cy, int radius) {
    // Crescent moon symbol + small "Z"
    gfx.fillCircle(cx - 4, cy, radius, TFT_BLACK);
    gfx.fillCircle(cx + 4, cy - 5, radius - 3, TFT_WHITE);

    // Sleep "Z" letter
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextSize(2);
    gfx.drawString("Z", cx + radius - 2, cy - radius / 2);
}

void UIEngine::drawLedIcon(M5GFX& gfx, int cx, int cy, bool active) {
    uint16_t color = active ? TFT_WHITE : TFT_BLACK;

    // LED bulb: rounded top dome + rectangular base + pins
    gfx.fillCircle(cx, cy - 4, 10, color);
    gfx.fillRect(cx - 10, cy - 4, 21, 10, color);
    gfx.fillRect(cx - 12, cy + 6, 25, 4, color);
    gfx.fillRect(cx - 6, cy + 10, 3, 9, color);
    gfx.fillRect(cx + 3, cy + 10, 3, 9, color);

    if (active) {
        // Radiant light rays around active LED
        gfx.drawLine(cx, cy - 18, cx, cy - 24, color);
        gfx.drawLine(cx - 1, cy - 18, cx - 1, cy - 24, color);
        gfx.drawLine(cx - 13, cy - 13, cx - 18, cy - 18, color);
        gfx.drawLine(cx + 13, cy - 13, cx + 18, cy - 18, color);
        gfx.drawLine(cx - 16, cy - 3, cx - 22, cy - 3, color);
        gfx.drawLine(cx + 16, cy - 3, cx + 22, cy - 3, color);
    } else {
        // Diagonal strike-through slash across disabled LED
        gfx.drawLine(cx - 16, cy + 16, cx + 16, cy - 16, TFT_BLACK);
        gfx.drawLine(cx - 16, cy + 17, cx + 16, cy - 15, TFT_BLACK);
        gfx.drawLine(cx - 16, cy + 15, cx + 16, cy - 17, TFT_BLACK);
    }
}

void UIEngine::drawSpeakerIcon(M5GFX& gfx, int cx, int cy, bool muted) {
    uint16_t color = muted ? TFT_BLACK : TFT_WHITE;

    int sx = cx - 6;
    gfx.fillRect(sx - 8, cy - 6, 6, 12, color);
    gfx.fillTriangle(sx - 3, cy - 6, sx + 5, cy - 14, sx + 5, cy + 14, color);

    if (!muted) {
        // Sound waves radiating to the right
        for (int r = 10; r <= 11; ++r) {
            gfx.drawCircle(sx + 5, cy, r, color);
        }
        for (int r = 17; r <= 18; ++r) {
            gfx.drawCircle(sx + 5, cy, r, color);
        }
        // Mask left side of wave circles
        gfx.fillRect(sx - 20, cy - 22, 25, 44, TFT_BLACK);
        // Re-fill speaker body over mask
        gfx.fillRect(sx - 8, cy - 6, 6, 12, color);
        gfx.fillTriangle(sx - 3, cy - 6, sx + 5, cy - 14, sx + 5, cy + 14, color);
    } else {
        // Muted 'X' symbol
        int xx = sx + 15;
        gfx.drawLine(xx - 5, cy - 6, xx + 5, cy + 6, color);
        gfx.drawLine(xx - 5, cy - 5, xx + 5, cy + 7, color);
        gfx.drawLine(xx - 5, cy + 6, xx + 5, cy - 6, color);
        gfx.drawLine(xx - 5, cy + 7, xx + 5, cy - 5, color);
    }
}

void UIEngine::drawQuickMenu(M5GFX& gfx) {
    // Panel hanging from the status bar: full-bleed (zero side margins, no side borders), square top
    // merging into the bar above, bottom closed by a thicker line with rounded corners.
    gfx.fillRect(QM_X, QM_Y, QM_W, QM_H - QM_R, TFT_WHITE);
    gfx.fillRoundRect(QM_X, QM_BOTTOM - 2 * QM_R, QM_W, 2 * QM_R, QM_R, TFT_WHITE);

    // Bottom border: solid QM_T px band built as the outer rounded shape minus its white interior
    // (two fillRoundRect keep the thickness uniform around the curve), then wipe the top edge and
    // the side bands so only the bottom outline is left.
    gfx.fillRoundRect(QM_X, QM_Y, QM_W, QM_H, QM_R, TFT_BLACK);
    gfx.fillRoundRect(QM_X + QM_T, QM_Y, QM_W - 2 * QM_T, QM_H - QM_T, QM_R - QM_T, TFT_WHITE);
    gfx.fillRect(QM_X, QM_Y, QM_W, QM_R + 1, TFT_WHITE);
    gfx.fillRect(QM_X, QM_Y + QM_R + 1, QM_T, QM_H - 2 * QM_R - 2, TFT_WHITE);
    gfx.fillRect(QM_X + QM_W - QM_T, QM_Y + QM_R + 1, QM_T, QM_H - 2 * QM_R - 2, TFT_WHITE);

    // ================= ROW 1: Power, Standby, Brightness =================
    // 1. Button Spegni (Power Off): X: 14, Y: 51, W: 74, H: 70
    int b1X = QM_COL1_X, b1Y = QM_ROW1_Y, b1W = QM_BTN_W, b1H = QM_BTN_H;
    gfx.drawRoundRect(b1X, b1Y, b1W, b1H, 6, TFT_BLACK);
    gfx.fillRoundRect(b1X + 1, b1Y + 1, b1W - 2, b1H - 2, 5, TFT_WHITE);
    drawPowerIcon(gfx, b1X + b1W / 2, b1Y + b1H / 2, 20);

    // 2. Button Standby: X: 98, Y: 51, W: 74, H: 70
    int b2X = QM_COL2_X, b2Y = QM_ROW1_Y, b2W = QM_BTN_W, b2H = QM_BTN_H;
    gfx.drawRoundRect(b2X, b2Y, b2W, b2H, 6, TFT_BLACK);
    gfx.fillRoundRect(b2X + 1, b2Y + 1, b2W - 2, b2H - 2, 5, TFT_WHITE);
    drawStandbyIcon(gfx, b2X + b2W / 2, b2Y + b2H / 2, 20);

    // 3. Brightness Control Box: X: 182, Y: 51, W: 284, H: 70
    int brX = QM_BOX_X, brY = QM_ROW1_Y, brW = QM_BOX_W, brH = QM_BTN_H;
    gfx.drawRoundRect(brX, brY, brW, brH, 6, TFT_BLACK);
    gfx.fillRoundRect(brX + 1, brY + 1, brW - 2, brH - 2, 5, TFT_WHITE);

    // [-] button: X: 190, Y: 57, W: 58, H: 58
    int bmX = QM_STEP_X1, bmY = QM_STEP_Y1, bmW = QM_STEP_W, bmH = QM_STEP_H;
    gfx.drawRoundRect(bmX, bmY, bmW, bmH, 4, TFT_BLACK);
    gfx.fillRoundRect(bmX + 1, bmY + 1, bmW - 2, bmH - 2, 3, TFT_WHITE);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextSize(3);
    gfx.drawString("-", bmX + bmW / 2, bmY + bmH / 2);

    // [+] button: X: 400, Y: 57, W: 58, H: 58
    int bpX = QM_STEP_X2, bpY = QM_STEP_Y1, bpW = QM_STEP_W, bpH = QM_STEP_H;
    gfx.drawRoundRect(bpX, bpY, bpW, bpH, 4, TFT_BLACK);
    gfx.fillRoundRect(bpX + 1, bpY + 1, bpW - 2, bpH - 2, 3, TFT_WHITE);
    gfx.setTextSize(3);
    gfx.drawString("+", bpX + bpW / 2, bpY + bpH / 2);

    // Center brightness %
    uint8_t curBright = MeshService::getInstance().getFrontlight();
    char brText[16];
    snprintf(brText, sizeof(brText), "%u%%", curBright);
    gfx.setTextSize(3);
    gfx.drawString(brText, brX + brW / 2, brY + brH / 2);

    // ================= ROW 2: LED Toggle, Buzzer Mute, Volume Slider =================
    // 4. LED Notifications Toggle: X: 14, Y: 131, W: 74, H: 70
    int ledX = QM_COL1_X, ledY = QM_ROW2_Y, ledW = QM_BTN_W, ledH = QM_BTN_H;
    bool ledActive = BSP::getInstance().getLedNotifications();
    gfx.drawRoundRect(ledX, ledY, ledW, ledH, 6, TFT_BLACK);
    gfx.fillRoundRect(ledX + 1, ledY + 1, ledW - 2, ledH - 2, 5, ledActive ? TFT_BLACK : TFT_WHITE);
    drawLedIcon(gfx, ledX + ledW / 2, ledY + ledH / 2, ledActive);

    // 5. Buzzer Mute Toggle: X: 98, Y: 131, W: 74, H: 70
    int mutX = QM_COL2_X, mutY = QM_ROW2_Y, mutW = QM_BTN_W, mutH = QM_BTN_H;
    uint8_t curVol = BSP::getInstance().getBuzzerVolume();
    bool soundActive = (curVol > 0);
    gfx.drawRoundRect(mutX, mutY, mutW, mutH, 6, TFT_BLACK);
    gfx.fillRoundRect(mutX + 1, mutY + 1, mutW - 2, mutH - 2, 5, soundActive ? TFT_BLACK : TFT_WHITE);
    drawSpeakerIcon(gfx, mutX + mutW / 2, mutY + mutH / 2, !soundActive);

    // 6. Buzzer Volume Box: X: 182, Y: 131, W: 284, H: 70
    int volX = QM_BOX_X, volY = QM_ROW2_Y, volW = QM_BOX_W, volH = QM_BTN_H;
    gfx.drawRoundRect(volX, volY, volW, volH, 6, TFT_BLACK);
    gfx.fillRoundRect(volX + 1, volY + 1, volW - 2, volH - 2, 5, TFT_WHITE);

    // Volume [-] button: X: 190, Y: 137, W: 58, H: 58
    int vmX = QM_STEP_X1, vmY = QM_STEP_Y2, vmW = QM_STEP_W, vmH = QM_STEP_H;
    gfx.drawRoundRect(vmX, vmY, vmW, vmH, 4, TFT_BLACK);
    gfx.fillRoundRect(vmX + 1, vmY + 1, vmW - 2, vmH - 2, 3, TFT_WHITE);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextSize(3);
    gfx.drawString("-", vmX + vmW / 2, vmY + vmH / 2);

    // Volume [+] button: X: 400, Y: 137, W: 58, H: 58
    int vpX = QM_STEP_X2, vpY = QM_STEP_Y2, vpW = QM_STEP_W, vpH = QM_STEP_H;
    gfx.drawRoundRect(vpX, vpY, vpW, vpH, 4, TFT_BLACK);
    gfx.fillRoundRect(vpX + 1, vpY + 1, vpW - 2, vpH - 2, 3, TFT_WHITE);
    gfx.setTextSize(3);
    gfx.drawString("+", vpX + vpW / 2, vpY + vpH / 2);

    // Center Volume Level indicator: "current/max" with the same font size as the brightness
    // percentage above (the old MUT/MIN/MED/MAX captions were dropped).
    char volText[16];
    snprintf(volText, sizeof(volText), "%u/3", (unsigned)curVol);
    gfx.setTextSize(3);
    gfx.drawString(volText, volX + volW / 2, volY + volH / 2);
}

bool UIEngine::handleQuickMenuTouch(const TouchEvent& ev) {
    // 1. Swipe left-to-right to close quick menu
    if (ev.type == TouchEventType::SwipeRight) {
        closeQuickMenu();
        return true;
    }

    // 2. Click on Bottom Nav Bar while Quick Menu is open:
    // Close quick menu AND navigate to tapped app/view directly (single refresh)!
    if (ev.y >= 730 && ev.y <= 800) {
        if (ev.type == TouchEventType::Click || ev.type == TouchEventType::Up) {
            _quickMenuOpen = false;
            EPDDriver::getInstance().setRefreshMode(RefreshMode::Grayscale);
            handleNavBarTouch(ev);
            _needsFullRedraw = true;
        }
        return true;
    }

    if (ev.type != TouchEventType::Click) return true; // consume touch

    // 3. Tap outside the panel to close (a tap on the status bar itself included)
    if (ev.x < QM_X || ev.x >= QM_X + QM_W || ev.y < QM_Y || ev.y >= QM_BOTTOM) {
        closeQuickMenu();
        return true;
    }

    // ================= ROW 1 TOUCH =================
    // 4. Power Off button (X: 14..88, Y: 51..121)
    if (ev.x >= QM_COL1_X && ev.x <= QM_COL1_X + QM_BTN_W &&
        ev.y >= QM_ROW1_Y && ev.y <= QM_ROW1_Y + QM_BTN_H) {
        _quickMenuOpen = false;
        MeshService::getInstance().saveConfig();
        BSP::getInstance().powerOff("Power Off");
        return true;
    }

    // 5. Standby button (X: 98..172, Y: 51..121)
    if (ev.x >= QM_COL2_X && ev.x <= QM_COL2_X + QM_BTN_W &&
        ev.y >= QM_ROW1_Y && ev.y <= QM_ROW1_Y + QM_BTN_H) {
        _quickMenuOpen = false;
        MeshService::getInstance().saveConfig();
        BSP::getInstance().enterLowPower(BSP::PowerMode::Standby);
        return true;
    }

    // 6. Brightness [-] (X: 190..248, Y: 57..115)
    if (ev.x >= QM_STEP_X1 && ev.x <= QM_STEP_X1 + QM_STEP_W &&
        ev.y >= QM_STEP_Y1 && ev.y <= QM_STEP_Y1 + QM_STEP_H) {
        uint8_t cur = MeshService::getInstance().getFrontlight();
        if (cur >= 10) cur -= 10;
        else cur = 0;
        MeshService::getInstance().setFrontlight(cur);
        BSP::getInstance().setFrontlight(cur);
        MeshService::getInstance().saveConfig(false);
        drawQuickMenu(M5.Display);
        EPDDriver::getInstance().flushRect(QM_X, QM_Y, QM_W, QM_H);
        return true;
    }

    // 7. Brightness [+] (X: 400..458, Y: 57..115)
    if (ev.x >= QM_STEP_X2 && ev.x <= QM_STEP_X2 + QM_STEP_W &&
        ev.y >= QM_STEP_Y1 && ev.y <= QM_STEP_Y1 + QM_STEP_H) {
        uint8_t cur = MeshService::getInstance().getFrontlight();
        if (cur <= 90) cur += 10;
        else cur = 100;
        MeshService::getInstance().setFrontlight(cur);
        BSP::getInstance().setFrontlight(cur);
        MeshService::getInstance().saveConfig(false);
        drawQuickMenu(M5.Display);
        EPDDriver::getInstance().flushRect(QM_X, QM_Y, QM_W, QM_H);
        return true;
    }

    // ================= ROW 2 TOUCH =================
    // 8. LED Notifications Toggle (X: 14..88, Y: 131..201)
    if (ev.x >= QM_COL1_X && ev.x <= QM_COL1_X + QM_BTN_W &&
        ev.y >= QM_ROW2_Y && ev.y <= QM_ROW2_Y + QM_BTN_H) {
        bool current = BSP::getInstance().getLedNotifications();
        BSP::getInstance().setLedNotifications(!current);
        MeshService::getInstance().saveConfig(false);
        drawQuickMenu(M5.Display);
        EPDDriver::getInstance().flushRect(QM_X, QM_Y, QM_W, QM_H);
        return true;
    }

    // 9. Buzzer Mute Toggle (X: 98..172, Y: 131..201)
    if (ev.x >= QM_COL2_X && ev.x <= QM_COL2_X + QM_BTN_W &&
        ev.y >= QM_ROW2_Y && ev.y <= QM_ROW2_Y + QM_BTN_H) {
        uint8_t curVol = BSP::getInstance().getBuzzerVolume();
        if (curVol > 0) {
            BSP::getInstance().setBuzzerVolume(0);
        } else {
            uint8_t restoreVol = BSP::getInstance().getLastNonZeroVolume();
            if (restoreVol == 0) restoreVol = 2;
            BSP::getInstance().setBuzzerVolume(restoreVol);
            BSP::getInstance().beep(BSP::NOTIFY_TONE_HZ, 60); // same tone as the RX notification
        }
        MeshService::getInstance().saveConfig(false);
        drawQuickMenu(M5.Display);
        EPDDriver::getInstance().flushRect(QM_X, QM_Y, QM_W, QM_H);
        return true;
    }

    // 10. Buzzer Volume [-] (X: 190..248, Y: 137..195)
    if (ev.x >= QM_STEP_X1 && ev.x <= QM_STEP_X1 + QM_STEP_W &&
        ev.y >= QM_STEP_Y2 && ev.y <= QM_STEP_Y2 + QM_STEP_H) {
        uint8_t curVol = BSP::getInstance().getBuzzerVolume();
        if (curVol > 0) {
            curVol--;
            BSP::getInstance().setBuzzerVolume(curVol);
            if (curVol > 0) {
                BSP::getInstance().beep(BSP::NOTIFY_TONE_HZ, 60); // same tone as the RX notification
            }
            MeshService::getInstance().saveConfig(false);
            drawQuickMenu(M5.Display);
            EPDDriver::getInstance().flushRect(QM_X, QM_Y, QM_W, QM_H);
        }
        return true;
    }

    // 11. Buzzer Volume [+] (X: 400..458, Y: 137..195)
    if (ev.x >= QM_STEP_X2 && ev.x <= QM_STEP_X2 + QM_STEP_W &&
        ev.y >= QM_STEP_Y2 && ev.y <= QM_STEP_Y2 + QM_STEP_H) {
        uint8_t curVol = BSP::getInstance().getBuzzerVolume();
        if (curVol < 3) {
            curVol++;
            BSP::getInstance().setBuzzerVolume(curVol);
            BSP::getInstance().beep(BSP::NOTIFY_TONE_HZ, 60); // same tone as the RX notification
            MeshService::getInstance().saveConfig(false);
            drawQuickMenu(M5.Display);
            EPDDriver::getInstance().flushRect(QM_X, QM_Y, QM_W, QM_H);
        }
        return true;
    }

    return true;
}

} // namespace MonoMesh
