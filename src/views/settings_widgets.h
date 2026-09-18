#pragma once

// Shared drawing/hit-testing primitives for the settings screen.
//
// WHY THIS FILE EXISTS
// The old settings tabs hardcoded every button rectangle twice (once in the draw path and once in
// the touch path), which is how the touch boxes ended up 14 px above the buttons (see the CANALE
// history in view_settings.cpp). Here the geometry lives in one place: a page describes its rows as
// a RowDef list, layoutRows() turns that into concrete rects, and both the draw and the touch code
// walk the same list. Fonts and touch targets are fixed the same way for every page:
//   * interactive labels are always text size 2 (the size the rest of the firmware uses),
//   * size 1 is only for secondary/read-only text,
//   * every tappable control is at least 44 px tall.

#include <Arduino.h>
#include <M5Unified.h>
#include "touch_manager.h"
#include "view_base.h"

namespace MonoMesh {
namespace SWSettings {

// ---------------------------------------------------------------- geometry
// CONTENT_X/W match the conversation list cards in view_chat (14 + 452 px): every list in the UI
// has the same box length.
constexpr int CONTENT_X = 14;
constexpr int CONTENT_W = 452;
constexpr int CONTENT_TOP = 82;      // first row, under the 32 px page header (44..76)
constexpr int CONTENT_BOTTOM = 729;  // nav bar starts at 730
constexpr int GAP = 6;
constexpr int ROW_H = 56;            // standard row (toggle / stepper / value / action)
constexpr int SLIDER_H = 72;         // slider and SD card rows
constexpr int BUTTON3_H = 64;        // three side-by-side action buttons
constexpr int ROOT_ROW_H = 64;       // category list rows (no subtitle: shorter than before)
constexpr int ROOT_GAP = 8;          // same spacing as the mesh node cards (80 + 8)
constexpr int HEADER_Y = 44;
constexpr int HEADER_H = 32;
constexpr int CORNER = 6;
constexpr int MAX_ROWS = 12;

enum class RowKind : uint8_t {
    Value,    // label left, value + chevron right  (opens a select modal / keyboard)
    Info,     // read-only: light gray card, no chevron
    Toggle,   // label left, switch right
    Slider,   // label + value on top, draggable track below (SLIDER_H)
    Stepper,  // label left, [-] value [+] on the right
    Action,   // full-width button row
    Buttons3, // one row with three action buttons (BUTTON3_H)
    SdCard    // label/value + usage bar, read-only (SLIDER_H)
};

struct RowDef {
    RowKind kind;
    const char* label;
};

struct RowRect {
    int y = 0;
    int h = ROW_H;
};

inline int rowHeight(RowKind k) {
    switch (k) {
        case RowKind::Slider:
        case RowKind::SdCard: return SLIDER_H;
        case RowKind::Buttons3: return BUTTON3_H;
        default: return ROW_H;
    }
}

// Fills `out` with the on-screen rect of every row, in the order they are drawn.
inline int layoutRows(const RowDef* defs, int n, RowRect* out) {
    int y = CONTENT_TOP;
    for (int i = 0; i < n && i < MAX_ROWS; ++i) {
        out[i].y = y;
        out[i].h = rowHeight(defs[i].kind);
        y += out[i].h + GAP;
    }
    return n;
}

inline bool inRect(const TouchEvent& ev, int x, int y, int w, int h) {
    return ev.x >= x && ev.x < x + w && ev.y >= y && ev.y < y + h;
}

inline bool inRow(const TouchEvent& ev, const RowRect& r) {
    return inRect(ev, CONTENT_X, r.y, CONTENT_W, r.h);
}

// Slider track: the whole row is draggable, so the touch code maps x -> position.
constexpr int SLIDER_INSET = 18;
inline int sliderPosFromX(int x, int steps) {
    const int tx = CONTENT_X + SLIDER_INSET;
    const int tw = CONTENT_W - 2 * SLIDER_INSET - 1; // same span the knob uses in sliderRow()
    int p = (int)(((x - tx) * (int64_t)steps + tw / 2) / tw);
    if (p < 0) p = 0;
    if (p > steps) p = steps;
    return p;
}

// Stepper controls (right-aligned: [-] [value] [+])
constexpr int STEPPER_BTN_W = 52;
constexpr int STEPPER_BTN_H = 44;
constexpr int STEPPER_VALUE_W = 140;
inline int stepperPlusX() { return CONTENT_X + CONTENT_W - 14 - STEPPER_BTN_W; }
inline int stepperValueX() { return stepperPlusX() - 6 - STEPPER_VALUE_W; }
inline int stepperMinusX() { return stepperValueX() - 6 - STEPPER_BTN_W; }
inline bool stepperHitMinus(const TouchEvent& ev, const RowRect& r) {
    return inRect(ev, stepperMinusX(), r.y + (r.h - STEPPER_BTN_H) / 2, STEPPER_BTN_W, STEPPER_BTN_H);
}
inline bool stepperHitPlus(const TouchEvent& ev, const RowRect& r) {
    return inRect(ev, stepperPlusX(), r.y + (r.h - STEPPER_BTN_H) / 2, STEPPER_BTN_W, STEPPER_BTN_H);
}

// ---------------------------------------------------------------- drawing
inline void card(M5GFX& g, int x, int y, int w, int h, uint16_t fill = TFT_WHITE) {
    g.drawRoundRect(x, y, w, h, CORNER, TFT_BLACK);
    g.fillRoundRect(x + 1, y + 1, w - 2, h - 2, CORNER - 1, fill);
}

inline void chevron(M5GFX& g, int cx, int cy, uint16_t color = TFT_BLACK) {
    // Three parallel strokes instead of two: a slightly bolder ">" as asked for the settings list.
    for (int i = 0; i < 3; ++i) {
        g.drawLine(cx - 3, cy - 8 + i, cx + 4, cy - i, color);
        g.drawLine(cx + 4, cy - i, cx - 3, cy + 8 - i, color);
    }
}

inline void switchControl(M5GFX& g, int x, int y, bool on) {
    const int w = 76, h = 34;
    if (on) {
        g.fillRoundRect(x, y, w, h, h / 2, TFT_BLACK);
        g.fillCircle(x + w - h / 2, y + h / 2, h / 2 - 6, TFT_WHITE);
    } else {
        g.drawRoundRect(x, y, w, h, h / 2, TFT_BLACK);
        g.fillRoundRect(x + 1, y + 1, w - 2, h - 2, h / 2 - 1, TFT_WHITE);
        g.fillCircle(x + h / 2, y + h / 2, h / 2 - 7, TFT_BLACK);
    }
}

// Read-only row: light gray so it is obvious that nothing happens on tap.
inline void infoRow(M5GFX& g, const RowRect& r, const char* label, const char* value) {
    card(g, CONTENT_X, r.y, CONTENT_W, r.h, TFT_LIGHTGRAY);
    g.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextSize(2);
    g.drawString(label, CONTENT_X + 16, r.y + r.h / 2);
    g.setTextDatum(textdatum_t::middle_right);
    g.setTextSize(2);
    if (g.textWidth(value) > 250) g.setTextSize(1);
    g.drawString(value, CONTENT_X + CONTENT_W - 16, r.y + r.h / 2);
}

inline void valueRow(M5GFX& g, const RowRect& r, const char* label, const char* value) {
    card(g, CONTENT_X, r.y, CONTENT_W, r.h);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextSize(2);
    g.drawString(label, CONTENT_X + 16, r.y + r.h / 2);
    g.setTextDatum(textdatum_t::middle_right);
    g.setTextSize(2);
    const int valueRight = CONTENT_X + CONTENT_W - 40;
    if (g.textWidth(value) > 200) g.setTextSize(1);
    g.drawString(value, valueRight, r.y + r.h / 2);
    chevron(g, CONTENT_X + CONTENT_W - 22, r.y + r.h / 2);
}

inline void toggleRow(M5GFX& g, const RowRect& r, const char* label, bool on) {
    card(g, CONTENT_X, r.y, CONTENT_W, r.h);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextSize(2);
    g.drawString(label, CONTENT_X + 16, r.y + r.h / 2);
    switchControl(g, CONTENT_X + CONTENT_W - 16 - 76, r.y + (r.h - 34) / 2, on);
}

inline void sliderRow(M5GFX& g, const RowRect& r, const char* label, const char* valueText,
                      int pos, int steps, bool ticks) {
    card(g, CONTENT_X, r.y, CONTENT_W, r.h);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextSize(2);
    g.drawString(label, CONTENT_X + 16, r.y + 22);
    g.setTextDatum(textdatum_t::middle_right);
    g.drawString(valueText, CONTENT_X + CONTENT_W - 16, r.y + 22);

    const int tx = CONTENT_X + SLIDER_INSET;
    const int tw = CONTENT_W - 2 * SLIDER_INSET;
    const int ty = r.y + r.h - 26;
    const int th = 12;
    g.drawRoundRect(tx, ty, tw, th, 6, TFT_BLACK);
    g.fillRoundRect(tx + 1, ty + 1, tw - 2, th - 2, 5, TFT_WHITE);
    const int knobX = tx + (int)((float)pos / (steps > 0 ? steps : 1) * (tw - 1));
    if (knobX > tx + 2) g.fillRect(tx + 2, ty + 2, knobX - tx - 2, th - 4, TFT_BLACK);
    if (ticks && steps > 1) {
        for (int i = 1; i < steps; ++i) {
            const int tickX = tx + (i * (tw - 1)) / steps;
            g.drawFastVLine(tickX, ty + 2, th - 4, TFT_BLACK);
        }
    }
    g.fillCircle(knobX, ty + th / 2, 13, TFT_BLACK);
    g.fillCircle(knobX, ty + th / 2, 10, TFT_WHITE);
    g.fillCircle(knobX, ty + th / 2, 5, TFT_BLACK);
}

inline void stepperRow(M5GFX& g, const RowRect& r, const char* label, const char* value,
                       bool canDown = true, bool canUp = true) {
    card(g, CONTENT_X, r.y, CONTENT_W, r.h);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextSize(2);
    g.drawString(label, CONTENT_X + 16, r.y + r.h / 2);

    const int by = r.y + (r.h - STEPPER_BTN_H) / 2;
    const uint16_t arrowBg = TFT_LIGHTGRAY;
    // [-]
    g.drawRoundRect(stepperMinusX(), by, STEPPER_BTN_W, STEPPER_BTN_H, 4, TFT_BLACK);
    g.fillRoundRect(stepperMinusX() + 1, by + 1, STEPPER_BTN_W - 2, STEPPER_BTN_H - 2, 3,
                    canDown ? arrowBg : TFT_WHITE);
    g.setTextColor(canDown ? TFT_BLACK : TFT_LIGHTGRAY, canDown ? arrowBg : TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextSize(3);
    g.drawString("-", stepperMinusX() + STEPPER_BTN_W / 2, by + STEPPER_BTN_H / 2);
    // [+]
    g.drawRoundRect(stepperPlusX(), by, STEPPER_BTN_W, STEPPER_BTN_H, 4, TFT_BLACK);
    g.fillRoundRect(stepperPlusX() + 1, by + 1, STEPPER_BTN_W - 2, STEPPER_BTN_H - 2, 3,
                    canUp ? arrowBg : TFT_WHITE);
    g.setTextColor(canUp ? TFT_BLACK : TFT_LIGHTGRAY, canUp ? arrowBg : TFT_WHITE);
    g.drawString("+", stepperPlusX() + STEPPER_BTN_W / 2, by + STEPPER_BTN_H / 2);
    // value
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextSize(2);
    g.drawString(value, stepperValueX() + STEPPER_VALUE_W / 2, r.y + r.h / 2);
}

inline void actionRow(M5GFX& g, const RowRect& r, const char* label) {
    card(g, CONTENT_X, r.y, CONTENT_W, r.h);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextSize(2);
    g.drawString(label, CONTENT_X + CONTENT_W / 2, r.y + r.h / 2);
}

inline void buttons3Row(M5GFX& g, const RowRect& r, const char* a, const char* b, const char* c) {
    const char* labels[3] = {a, b, c};
    const int bw = 148;
    const int gap = (CONTENT_W - 3 * bw) / 2; // 8
    for (int i = 0; i < 3; ++i) {
        const int bx = CONTENT_X + i * (bw + gap);
        g.drawRoundRect(bx, r.y, bw, r.h, CORNER, TFT_BLACK);
        g.fillRoundRect(bx + 1, r.y + 1, bw - 2, r.h - 2, CORNER - 1, TFT_WHITE);
        g.setTextColor(TFT_BLACK, TFT_WHITE);
        g.setTextDatum(textdatum_t::middle_center);
        g.setTextSize(2);
        g.drawString(labels[i], bx + bw / 2, r.y + r.h / 2);
    }
}

inline int buttons3Hit(const TouchEvent& ev, const RowRect& r) {
    const int bw = 148;
    const int gap = (CONTENT_W - 3 * bw) / 2;
    for (int i = 0; i < 3; ++i) {
        const int bx = CONTENT_X + i * (bw + gap);
        if (inRect(ev, bx, r.y, bw, r.h)) return i;
    }
    return -1;
}

inline void sdCardRow(M5GFX& g, const RowRect& r, const char* value, float fraction, bool mounted) {
    card(g, CONTENT_X, r.y, CONTENT_W, r.h);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextSize(2);
    g.drawString("SD card", CONTENT_X + 16, r.y + 22);
    g.setTextDatum(textdatum_t::middle_right);
    g.drawString(value, CONTENT_X + CONTENT_W - 16, r.y + 22);

    const int bx = CONTENT_X + 16;
    const int bw = CONTENT_W - 32;
    const int by = r.y + r.h - 26;
    const int bh = 14;
    g.drawRect(bx, by, bw, bh, TFT_BLACK);
    if (mounted) {
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        const int fillW = (int)(fraction * (bw - 2));
        if (fillW > 0) g.fillRect(bx + 1, by + 1, fillW, bh - 2, TFT_BLACK);
    }
}

// ---------------------------------------------------------------- header
// No back button: the settings page is left with a left-to-right swipe (and BtnA). The gear that
// used to sit on the right cluttered the bar without adding anything.
// Gray bar (black dot screen, black text) like every other view header.
inline void header(M5GFX& g, const char* title) {
    headerBackground(g, HEADER_Y, HEADER_H);
    g.drawFastHLine(0, HEADER_Y + HEADER_H, 480, TFT_BLACK);
    g.setTextColor(TFT_BLACK);
    g.setTextDatum(textdatum_t::middle_left);
    g.setTextSize(2);
    // Header text: 4 px inside the card edge (CONTENT_X), so it lines up with the card content and
    // not with the 1 px border.
    g.drawString(title, CONTENT_X + 4, HEADER_Y + HEADER_H / 2);
}

// ---------------------------------------------------------------- category icons
enum class RootIcon : uint8_t { System = 0, Radio, Channels, Node, Mesh, Clock, Storage };

inline void rootIcon(M5GFX& g, RootIcon kind, int cx, int cy, uint16_t color, uint16_t bg) {
    switch (kind) {
        case RootIcon::System: { // two sliders
            g.drawFastHLine(cx - 14, cy - 7, 28, color);
            g.drawFastHLine(cx - 14, cy + 7, 28, color);
            g.fillCircle(cx - 6, cy - 7, 5, color);
            g.fillCircle(cx + 7, cy + 7, 5, color);
            break;
        }
        case RootIcon::Radio: { // antenna with waves
            g.fillTriangle(cx, cy + 2, cx - 10, cy - 12, cx + 10, cy - 12, color);
            g.fillRect(cx - 2, cy - 10, 4, 20, color);
            g.drawCircle(cx, cy + 10, 3, color);
            g.drawCircle(cx, cy + 10, 7, color);
            break;
        }
        case RootIcon::Channels: { // key
            g.drawCircle(cx - 7, cy - 6, 7, color);
            g.fillCircle(cx - 7, cy - 6, 3, bg);
            g.drawCircle(cx - 7, cy - 6, 7, color);
            g.drawLine(cx - 2, cy - 1, cx + 12, cy + 13, color);
            g.drawLine(cx - 1, cy - 2, cx + 13, cy + 12, color);
            g.drawLine(cx + 6, cy + 6, cx + 11, cy + 11, color);
            g.drawLine(cx + 9, cy + 3, cx + 14, cy + 8, color);
            break;
        }
        case RootIcon::Node: { // person
            g.drawCircle(cx, cy - 6, 7, color);
            g.drawCircle(cx, cy - 6, 6, color);
            for (int i = 0; i < 2; ++i) {
                g.drawLine(cx - 13, cy + 14 + i, cx - 8, cy + 2 + i, color);
                g.drawLine(cx + 13, cy + 14 + i, cx + 8, cy + 2 + i, color);
                g.drawLine(cx - 8, cy + 3 + i, cx + 8, cy + 3 + i, color);
            }
            break;
        }
        case RootIcon::Mesh: { // three linked nodes
            g.drawLine(cx, cy - 9, cx - 11, cy + 9, color);
            g.drawLine(cx + 1, cy - 9, cx - 10, cy + 9, color);
            g.drawLine(cx, cy - 9, cx + 11, cy + 9, color);
            g.drawLine(cx - 1, cy - 9, cx + 10, cy + 9, color);
            g.drawLine(cx - 11, cy + 9, cx + 11, cy + 9, color);
            g.fillCircle(cx, cy - 9, 4, color);
            g.fillCircle(cx - 11, cy + 9, 4, color);
            g.fillCircle(cx + 11, cy + 9, 4, color);
            break;
        }
        case RootIcon::Clock: { // clock face
            g.drawCircle(cx, cy, 13, color);
            g.drawCircle(cx, cy, 12, color);
            g.fillRect(cx - 2, cy - 8, 4, 10, color);
            g.fillRect(cx - 1, cy - 1, 9, 4, color);
            break;
        }
        case RootIcon::Storage: { // SD card
            g.drawRoundRect(cx - 10, cy - 13, 20, 26, 3, color);
            g.drawRoundRect(cx - 9, cy - 12, 18, 24, 2, color);
            g.fillRect(cx - 8, cy - 13, 16, 8, bg);
            g.fillRect(cx - 6, cy - 12, 2, 6, color);
            g.fillRect(cx - 2, cy - 12, 2, 6, color);
            g.fillRect(cx + 2, cy - 12, 2, 6, color);
            g.fillRect(cx - 6, cy + 1, 12, 9, color);
            break;
        }
    }
}

// ---------------------------------------------------------------- modals
// Confirmation dialog (destructive actions and power/reboot).
constexpr int CONFIRM_X = 40;
constexpr int CONFIRM_Y = 250;
constexpr int CONFIRM_W = 400;
constexpr int CONFIRM_H = 250;
constexpr int MODAL_BTN_H = 56;
inline int modalCancelX() { return CONFIRM_X + 18; }
inline int modalOkX() { return CONFIRM_X + CONFIRM_W - 18 - 170; }
inline int modalBtnY() { return CONFIRM_Y + CONFIRM_H - 18 - MODAL_BTN_H; }
constexpr int MODAL_BTN_W = 170;

inline void modalCard(M5GFX& g, int x, int y, int w, int h, const char* title) {
    g.fillRoundRect(x, y, w, h, 8, TFT_WHITE);
    g.drawRoundRect(x, y, w, h, 8, TFT_BLACK);
    g.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, TFT_BLACK);
    g.fillRect(x + 2, y + 2, w - 4, 38, TFT_LIGHTGRAY);
    g.drawFastHLine(x + 2, y + 40, w - 4, TFT_BLACK);
    g.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextSize(2);
    g.drawString(title, x + w / 2, y + 21);
}

inline void confirmModal(M5GFX& g, const char* title, const char* message) {
    modalCard(g, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, title);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_center);
    g.setTextSize(2);
    // Draw the message line by line (max two lines): no dependency on the library's newline handling.
    const char* nl = message ? strchr(message, '\n') : nullptr;
    if (nl) {
        char line1[64];
        size_t len = (size_t)(nl - message);
        if (len >= sizeof(line1)) len = sizeof(line1) - 1;
        memcpy(line1, message, len);
        line1[len] = '\0';
        g.drawString(line1, CONFIRM_X + CONFIRM_W / 2, CONFIRM_Y + 84);
        g.drawString(nl + 1, CONFIRM_X + CONFIRM_W / 2, CONFIRM_Y + 116);
    } else {
        g.drawString(message, CONFIRM_X + CONFIRM_W / 2, CONFIRM_Y + 96);
    }
    // Cancel
    const int by = modalBtnY();
    g.drawRoundRect(modalCancelX(), by, MODAL_BTN_W, MODAL_BTN_H, 6, TFT_BLACK);
    g.fillRoundRect(modalCancelX() + 1, by + 1, MODAL_BTN_W - 2, MODAL_BTN_H - 2, 5, TFT_LIGHTGRAY);
    g.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    g.drawString("CANCEL", modalCancelX() + MODAL_BTN_W / 2, by + MODAL_BTN_H / 2);
    // Confirm
    g.drawRoundRect(modalOkX(), by, MODAL_BTN_W, MODAL_BTN_H, 6, TFT_BLACK);
    g.fillRoundRect(modalOkX() + 1, by + 1, MODAL_BTN_W - 2, MODAL_BTN_H - 2, 5, TFT_BLACK);
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawString("CONFIRM", modalOkX() + MODAL_BTN_W / 2, by + MODAL_BTN_H / 2);
}

// Selection list ("phone style" picker). Height depends on the number of options.
constexpr int SEL_X = 30;
constexpr int SEL_TOP = 150;
constexpr int SEL_W = 420;
constexpr int SEL_ROW_H = 52;
constexpr int SEL_HEAD_H = 44;
inline int selHeight(int n) { return SEL_HEAD_H + n * SEL_ROW_H + 12; }
inline int selRowY(int i) { return SEL_TOP + SEL_HEAD_H + i * SEL_ROW_H; }

inline void selectModal(M5GFX& g, const char* title, const char* const* labels, int count, int selected) {
    const int h = selHeight(count);
    modalCard(g, SEL_X, SEL_TOP, SEL_W, h, title);
    for (int i = 0; i < count; ++i) {
        const int ry = selRowY(i);
        const bool on = (selected >= 0 && i == selected);
        g.drawRoundRect(SEL_X + 12, ry + 2, SEL_W - 24, SEL_ROW_H - 4, 5, TFT_BLACK);
        g.fillRoundRect(SEL_X + 13, ry + 3, SEL_W - 26, SEL_ROW_H - 6, 4, on ? TFT_BLACK : TFT_WHITE);
        g.setTextColor(on ? TFT_WHITE : TFT_BLACK, on ? TFT_BLACK : TFT_WHITE);
        g.setTextDatum(textdatum_t::middle_left);
        g.setTextSize(2);
        g.drawString(labels[i], SEL_X + 32, ry + SEL_ROW_H / 2);
        if (on) {
            // check mark
            const int cx = SEL_X + SEL_W - 40;
            const int cy = ry + SEL_ROW_H / 2;
            g.drawLine(cx - 7, cy, cx - 2, cy + 6, TFT_WHITE);
            g.drawLine(cx - 7, cy + 1, cx - 2, cy + 7, TFT_WHITE);
            g.drawLine(cx - 2, cy + 6, cx + 8, cy - 7, TFT_WHITE);
            g.drawLine(cx - 2, cy + 7, cx + 8, cy - 6, TFT_WHITE);
        }
    }
}

inline int selectModalHit(const TouchEvent& ev, int count) {
    if (!inRect(ev, SEL_X, SEL_TOP, SEL_W, selHeight(count))) return -1;
    for (int i = 0; i < count; ++i) {
        if (inRect(ev, SEL_X + 12, selRowY(i) + 2, SEL_W - 24, SEL_ROW_H - 4)) return i;
    }
    return -1;
}

inline bool confirmHitCancel(const TouchEvent& ev) {
    return inRect(ev, modalCancelX(), modalBtnY(), MODAL_BTN_W, MODAL_BTN_H);
}
inline bool confirmHitOk(const TouchEvent& ev) {
    return inRect(ev, modalOkX(), modalBtnY(), MODAL_BTN_W, MODAL_BTN_H);
}
inline bool inConfirmCard(const TouchEvent& ev) {
    return inRect(ev, CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H);
}

} // namespace SWSettings
} // namespace MonoMesh
