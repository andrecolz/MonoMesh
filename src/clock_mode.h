#pragma once

#include <Arduino.h>
#include <M5Unified.h>

namespace MonoMesh {

// One rasterized clock glyph: 1 bit per pixel, one row per line, MSB first and every row padded to
// a byte. The bitmap's top-left corner is drawn at (pen + xOffset, baseline + yOffset), where the
// pen is the left end of the baseline (yOffset is negative above the baseline).
// The glyphs come from tools/gen_clock_font.py, rendered at the final pixel size: nothing is scaled
// by the GFX bitmap font engine, which is what made the first implementation look pixelated.
struct ClockGlyph {
    const uint8_t* bitmap;
    int16_t width;
    int16_t height;
    int16_t xOffset;
    int16_t yOffset;
};

// Big HH:MM clock face for the CLOCK and STANDBY+CLOCK power modes.
//
// Refresh policy: the digits that changed are pushed with a 1-bit partial refresh (no flash), and
// every FULL_CLEAN_EVERY partial updates a full quality refresh (4 gray levels) wipes the ghosting
// the differential waveform accumulates. The whole face always lives in the M5GFX canvas, so a
// partial flush only limits what the panel waveform touches - never what the canvas contains.
class ClockMode {
public:
    static ClockMode& getInstance() {
        static ClockMode instance;
        return instance;
    }

    // Complete face + full-quality refresh. landscape selects the 800x480 orientation, flip180 turns
    // the face upside down (for a device mounted the other way): both are software only on the
    // SSD1677, nothing else on the board changes.
    void drawFace(bool landscape, bool flip180);

    // Reads the RTC and, only when the minute changed, redraws the changed digits and refreshes.
    // Cheap at every low-power wake: it is two I2C reads when nothing changed.
    void maybeRefresh(bool landscape, bool flip180);

    // Milliseconds to the next RTC minute boundary, capped at maxSliceMs, so the clock sleep slices
    // land on the minute change instead of polling it. Never returns 0.
    uint32_t msToNextMinute(uint32_t maxSliceMs) const;

    // Forget the last drawn time (leaving the power mode).
    void reset() {
        _lastValid = false;
        _lastHour = -1;
        _lastMinute = -1;
        _partials = 0;
    }

    static constexpr uint8_t FULL_CLEAN_EVERY = 10;

private:
    ClockMode() = default;
    ~ClockMode() = default;

    bool _lastValid = false;
    int16_t _lastHour = -1;
    int16_t _lastMinute = -1;
    uint8_t _partials = 0;

    // One generated font (see clock_font_large.h / clock_font_small.h).
    struct Face {
        const ClockGlyph* digits; // 10 entries, index = digit value
        const ClockGlyph* colon;
        int digitHeight;
        int digitAdvance;
        int colonAdvance;
    };

    // Everything needed to draw/refresh the face at its current orientation.
    struct Layout {
        Face face;
        int centerX;
        int centerY;
        int x0;       // pen of the first hour digit
        int baseline;
    };

    Layout layoutFor(bool landscape) const;
    bool applyOrientation(bool landscape, bool flip180);
    void drawTime(M5GFX& gfx, const Layout& l, int hour, int minute, bool valid,
                  bool hourChanged, bool minuteChanged);
    void refreshRegion(const Layout& l, bool hourChanged, bool minuteChanged);
};

} // namespace MonoMesh
