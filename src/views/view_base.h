#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include <cstddef>
#include <cstring>
#include "../touch_manager.h"

namespace MonoMesh {

// Network texts (node names, status strings, message bodies, channel labels learned from peers) and
// the values recovered from config.json/NVS are untrusted byte sequences. The GLCD font of M5GFX
// maps every byte 0x00..0xFF to a glyph and shifts 0xB0..0xFF by one for CP437 compatibility, so
// byte 0xFF makes it read one glyph (5 bytes) past the 1280-byte font table; control bytes are
// silently skipped by the renderer but still make the layout math disagree with what is drawn.
// Copy at most cap-1 bytes of `src` into `out`, keeping printable ASCII and valid UTF-8 sequences,
// keeping '\n' (the chat bubble wrapper uses it as a line break), dropping '\r' and turning every
// other control/invalid byte into '?'. Always NUL-terminates (when cap > 0) and returns the length.
inline size_t sanitizeDisplayText(const char* src, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (!src) {
        out[0] = '\0';
        return 0;
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
    size_t n = 0;
    while (*p != 0) {
        const unsigned char c = *p;
        if (c == '\r') {
            ++p;
            continue;
        }
        if (c == '\n') {
            if (n + 1 >= cap) break;
            out[n++] = '\n';
            ++p;
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            if (n + 1 >= cap) break;
            out[n++] = '?';
            ++p;
            continue;
        }
        if (c < 0x80) {
            if (n + 1 >= cap) break;
            out[n++] = static_cast<char>(c);
            ++p;
            continue;
        }
        // UTF-8: only accept a well-formed sequence (the bytes before the NUL are always readable,
        // because a truncated sequence ends on the terminator itself).
        int extra = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
        }
        bool valid = extra != 0;
        for (int i = 1; valid && i <= extra; ++i) {
            if ((p[i] & 0xC0) != 0x80) valid = false;
        }
        // The only code point that still hits the M5GFX shift bug is U+00FF (encoded C3 BF): the
        // renderer maps it to glyph 256 and reads past the font table, so it becomes '?' too.
        if (valid && extra == 1 && c == 0xC3 && p[1] == 0xBF) valid = false;
        if (valid && n + static_cast<size_t>(extra) + 1 < cap) {
            for (int i = 0; i <= extra; ++i) out[n++] = static_cast<char>(p[i]);
            p += extra + 1;
        } else {
            if (n + 1 >= cap) break;
            out[n++] = '?';
            ++p;
        }
    }
    out[n] = '\0';
    return n;
}

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
