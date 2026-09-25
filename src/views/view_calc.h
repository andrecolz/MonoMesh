#pragma once

#include <string>

#include "view_base.h"

namespace MonoMesh {

// CALC view: basic four-operation calculator with parentheses and percent, in expression mode.
// The user types an expression on a 4x5 keypad; '=' evaluates it with operator precedence and the
// result replaces it. The display zone (small expression line + big current value with caret) is
// the only part refreshed per key, with a fast 1-bit partial; the keypad is static and is drawn
// once by the full 4-level redraw of the UI engine, so its fills may use the two gray levels.
class ViewCalc : public ViewBase {
public:
    ViewCalc() = default;
    ~ViewCalc() override = default;

    void onEnter() override;
    void draw(M5GFX& gfx) override;
    bool handleTouch(const TouchEvent& ev) override;
    void update() override;

private:
    // Expression as typed, with display characters ('x' multiply, '.' decimal separator).
    std::string _expr;
    // Expression that produced the shown result; empty while typing.
    std::string _resultLine;
    // True right after '=': a digit starts a new calculation, an operator continues from the result.
    bool _showingResult = false;
    // Anti-ghosting bookkeeping for the display zone.
    uint32_t _lastKeyMs = 0;
    uint16_t _partialsSinceSettle = 0;
    bool _settlePending = false;

    void drawDisplay(M5GFX& gfx);
    void drawKeypad(M5GFX& gfx);
    void refreshDisplay();
    void pressKey(int index);

    // Input helpers.
    // `startNewNumber` = true for the keys that begin a new numeric literal (digit, dot): after a
    // result they must clear the expression instead of appending to the result text.
    void startFreshIfResult(bool startNewNumber);
    void appendDigit(char d);
    void appendOperator(char op);
    void appendDot();
    void appendPercent();
    void appendParen();
    void backspace();
    void clearAll();
    void equals();

    std::string currentNumber() const;
    bool currentNumberHasDot() const;
    int digitsInCurrentNumber() const;
};

} // namespace MonoMesh
