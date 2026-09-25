#include "view_calc.h"

#include "../calc_engine.h"
#include "../epd_driver.h"
#include "settings_widgets.h"

namespace MonoMesh {

using namespace SWSettings;

namespace {

// ---- display geometry ----
constexpr int DISPLAY_TOP = 84;
constexpr int DISPLAY_H = 150;
constexpr int EXPR_Y = 104;      // small expression line
constexpr int VALUE_Y = 185;     // big current value / result
constexpr int VALUE_RIGHT = 448; // right edge of the value, leaving room for the caret
constexpr int CARET_X = 454;
constexpr int CARET_W = 3;
constexpr int CARET_H = 40;

// ---- keypad geometry ----
constexpr int KEY_X = 14;
constexpr int KEY_W = 107;
constexpr int KEY_H = 88;
constexpr int KEY_GAP = 8;
constexpr int KEY_TOP = 248;
constexpr int KEY_STEP_X = KEY_W + KEY_GAP; // 4 columns: 14 .. 466 like every list card
constexpr int KEY_STEP_Y = KEY_H + KEY_GAP;
constexpr int NUM_KEYS = 20;

// ---- input limits / anti-ghosting ----
constexpr size_t MAX_EXPR = 64;
constexpr int MAX_DIGITS = 15;
constexpr uint16_t SETTLE_AFTER = 25;   // display partials before an idle 4-level settle
constexpr uint32_t SETTLE_QUIET_MS = 900;

enum class KeyKind : uint8_t { Digit, Operator, Clear, Backspace, Percent, Paren, Dot, Equals };

struct KeyDef {
    KeyKind kind;
    const char* label; // nullptr for the vector glyphs (backspace, operators, equals)
};

// Left to right, top to bottom; same layout as a phone calculator (C backspace % divide / 7 8 9 x
// / 4 5 6 - / 1 2 3 + / ( ) 0 . =).
const KeyDef KEYS[NUM_KEYS] = {
    {KeyKind::Clear, "C"},     {KeyKind::Backspace, nullptr}, {KeyKind::Percent, "%"}, {KeyKind::Operator, "/"},
    {KeyKind::Digit, "7"},     {KeyKind::Digit, "8"},         {KeyKind::Digit, "9"},   {KeyKind::Operator, "x"},
    {KeyKind::Digit, "4"},     {KeyKind::Digit, "5"},         {KeyKind::Digit, "6"},   {KeyKind::Operator, "-"},
    {KeyKind::Digit, "1"},     {KeyKind::Digit, "2"},         {KeyKind::Digit, "3"},   {KeyKind::Operator, "+"},
    {KeyKind::Paren, "()"},    {KeyKind::Digit, "0"},         {KeyKind::Dot, "."},     {KeyKind::Equals, nullptr}
};

bool isOperatorChar(char c) {
    return c == '+' || c == '-' || c == 'x' || c == '/';
}

bool isDigitChar(char c) {
    return c >= '0' && c <= '9';
}

void keyRect(int index, int& x, int& y) {
    x = KEY_X + (index % 4) * KEY_STEP_X;
    y = KEY_TOP + (index / 4) * KEY_STEP_Y;
}

// Backspace: pentagon outline with an X inside, like the reference keypad.
void drawBackspaceGlyph(M5GFX& g, int cx, int cy, uint16_t color) {
    const int tipX = cx - 17;
    const int leftX = cx - 8;
    const int rightX = cx + 16;
    const int topY = cy - 12;
    const int botY = cy + 12;
    for (int i = 0; i < 2; ++i) {
        g.drawLine(tipX, cy + i, leftX, topY + i, color);
        g.drawLine(tipX, cy + i, leftX, botY + i, color);
        g.drawLine(leftX, topY + i, rightX, topY + i, color);
        g.drawLine(leftX, botY + i, rightX, botY + i, color);
        g.drawLine(rightX, topY + i, rightX, botY + i, color);
    }
    for (int i = 0; i < 2; ++i) {
        g.drawLine(cx, cy - 5 + i, cx + 8, cy + 5 + i, color);
        g.drawLine(cx, cy + 5 + i, cx + 8, cy - 5 + i, color);
    }
}

// Vector operator glyphs: the firmware font is ASCII, so 'x' and '/' are drawn by hand.
void drawOperatorGlyph(M5GFX& g, char op, int cx, int cy, uint16_t color) {
    switch (op) {
        case '+':
            g.fillRect(cx - 16, cy - 2, 33, 5, color);
            g.fillRect(cx - 2, cy - 16, 5, 33, color);
            break;
        case '-':
            g.fillRect(cx - 16, cy - 2, 33, 5, color);
            break;
        case 'x':
            for (int i = -1; i <= 1; ++i) {
                g.drawLine(cx - 15 + i, cy - 15, cx + 15 + i, cy + 15, color);
                g.drawLine(cx - 15 + i, cy + 15, cx + 15 + i, cy - 15, color);
            }
            break;
        case '/': // division sign: bar with a dot above and below
            g.fillRect(cx - 15, cy - 2, 31, 5, color);
            g.fillCircle(cx, cy - 11, 3, color);
            g.fillCircle(cx, cy + 11, 3, color);
            break;
        default:
            break;
    }
}

void drawEqualsGlyph(M5GFX& g, int cx, int cy, uint16_t color) {
    g.fillRect(cx - 16, cy - 9, 33, 5, color);
    g.fillRect(cx - 16, cy + 4, 33, 5, color);
}

} // namespace

// ---------------------------------------------------------------- lifecycle
void ViewCalc::onEnter() {
    // The expression survives a switch away and back (RAM only, no persistence): only the
    // anti-ghosting bookkeeping restarts, because entering does a full 4-level redraw.
    _lastKeyMs = millis();
    _partialsSinceSettle = 0;
    _settlePending = false;
}

// ---------------------------------------------------------------- draw
void ViewCalc::draw(M5GFX& gfx) {
    gfx.fillRect(0, 44, 480, 686, TFT_WHITE);
    header(gfx, "CALC");
    drawDisplay(gfx);
    drawKeypad(gfx);
}

void ViewCalc::drawDisplay(M5GFX& gfx) {
    gfx.fillRect(0, DISPLAY_TOP, 480, DISPLAY_H, TFT_WHITE);

    // Small line: the expression being typed, or the one that produced the shown result.
    const std::string& top = _showingResult ? _resultLine : _expr;
    if (!top.empty()) {
        std::string shown = top;
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_right);
        gfx.setTextSize(2);
        // Long expression: show the tail that fits (the big line keeps the current number).
        while (shown.size() > 1 && gfx.textWidth(shown.c_str()) > 448) shown.erase(0, 1);
        gfx.drawString(shown.c_str(), 466, EXPR_Y);
    }

    // Big line: the number being typed (0 after an operator), or the result.
    std::string value;
    if (_showingResult) {
        value = _expr;
    } else {
        value = currentNumber();
        if (value.empty()) value = "0";
    }
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_right);
    float size = 6.0f;
    gfx.setTextSize(size);
    while (size > 3.0f && gfx.textWidth(value.c_str()) > VALUE_RIGHT - 18) {
        size -= 0.5f;
        gfx.setTextSize(size);
    }
    gfx.drawString(value.c_str(), VALUE_RIGHT, VALUE_Y);

    if (!_showingResult) {
        gfx.fillRect(CARET_X, VALUE_Y - CARET_H / 2, CARET_W, CARET_H, TFT_BLACK);
    }
}

void ViewCalc::drawKeypad(M5GFX& gfx) {
    for (int i = 0; i < NUM_KEYS; ++i) {
        int x = 0, y = 0;
        keyRect(i, x, y);
        const KeyDef& key = KEYS[i];

        uint16_t fill = TFT_WHITE;
        uint16_t fg = TFT_BLACK;
        switch (key.kind) {
            case KeyKind::Clear:
            case KeyKind::Backspace:
                fill = TFT_LIGHTGRAY;
                break;
            case KeyKind::Operator:
                fill = TFT_DARKGRAY;
                fg = TFT_WHITE;
                break;
            case KeyKind::Equals:
                fill = TFT_BLACK;
                fg = TFT_WHITE;
                break;
            default:
                break;
        }

        card(gfx, x, y, KEY_W, KEY_H, fill);
        const int cx = x + KEY_W / 2;
        const int cy = y + KEY_H / 2;
        switch (key.kind) {
            case KeyKind::Backspace:
                drawBackspaceGlyph(gfx, cx, cy, fg);
                break;
            case KeyKind::Operator:
                drawOperatorGlyph(gfx, key.label[0], cx, cy, fg);
                break;
            case KeyKind::Equals:
                drawEqualsGlyph(gfx, cx, cy, fg);
                break;
            default:
                gfx.setTextColor(fg, fill);
                gfx.setTextDatum(textdatum_t::middle_center);
                gfx.setTextSize(4);
                gfx.drawString(key.label, cx, cy);
                break;
        }
    }
}

// ---------------------------------------------------------------- refresh
void ViewCalc::refreshDisplay() {
    drawDisplay(M5.Display);
    EPDDriver::getInstance().flushRect(0, DISPLAY_TOP, 480, DISPLAY_H);
    _lastKeyMs = millis();
    if (_partialsSinceSettle < 0xFFFF) ++_partialsSinceSettle;
    if (_partialsSinceSettle >= SETTLE_AFTER) _settlePending = true;
}

void ViewCalc::update() {
    // Idle settle: after a burst of 1-bit partials a 4-level pass on the display zone removes the
    // ghosting without a full-screen flash (the keypad never changes, so it does not need one).
    if (_settlePending && (uint32_t)(millis() - _lastKeyMs) >= SETTLE_QUIET_MS) {
        _settlePending = false;
        _partialsSinceSettle = 0;
        EPDDriver::getInstance().flushRectGrayscale(0, DISPLAY_TOP, 480, DISPLAY_H);
    }
}

// ---------------------------------------------------------------- input
std::string ViewCalc::currentNumber() const {
    size_t start = _expr.size();
    while (start > 0) {
        const char c = _expr[start - 1];
        if (isDigitChar(c) || c == '.') {
            --start;
            continue;
        }
        break;
    }
    return _expr.substr(start);
}

bool ViewCalc::currentNumberHasDot() const {
    for (size_t i = _expr.size(); i > 0; --i) {
        const char c = _expr[i - 1];
        if (c == '.') return true;
        if (!isDigitChar(c)) break;
    }
    return false;
}

int ViewCalc::digitsInCurrentNumber() const {
    int n = 0;
    for (size_t i = _expr.size(); i > 0; --i) {
        const char c = _expr[i - 1];
        if (isDigitChar(c)) {
            ++n;
            continue;
        }
        if (c == '.') continue;
        break;
    }
    return n;
}

void ViewCalc::startFreshIfResult() {
    if (!_showingResult) return;
    _showingResult = false;
    _resultLine.clear();
    if (_expr == "Error") _expr.clear();
}

void ViewCalc::appendDigit(char d) {
    startFreshIfResult();
    if (_expr.size() >= MAX_EXPR) return;
    if (digitsInCurrentNumber() >= MAX_DIGITS) return;
    _expr += d;
}

void ViewCalc::appendOperator(char op) {
    startFreshIfResult();
    if (_expr.size() >= MAX_EXPR) return;
    if (_expr.empty()) {
        if (op == '-') _expr += op; // leading unary minus
        return;
    }
    const char last = _expr.back();
    if (isOperatorChar(last)) {
        // A '-' after 'x' or '/' is a unary sign (5x-3); every other pair replaces the operator.
        if (op == '-' && (last == 'x' || last == '/')) {
            _expr += op;
        } else {
            _expr.back() = op;
        }
        return;
    }
    if (last == '(') {
        if (op == '-') _expr += op;
        return;
    }
    _expr += op;
}

void ViewCalc::appendDot() {
    startFreshIfResult();
    if (_expr.size() >= MAX_EXPR) return;
    const char last = _expr.empty() ? '\0' : _expr.back();
    if (isDigitChar(last)) {
        if (currentNumberHasDot()) return;
        _expr += '.';
        return;
    }
    if (last == '.' || last == ')' || last == '%') return;
    _expr += "0.";
}

void ViewCalc::appendPercent() {
    startFreshIfResult();
    if (_expr.empty() || _expr.size() >= MAX_EXPR) return;
    const char last = _expr.back();
    if (isDigitChar(last) || last == ')' || last == '%') _expr += '%';
}

void ViewCalc::appendParen() {
    startFreshIfResult();
    if (_expr.size() >= MAX_EXPR) return;
    const char last = _expr.empty() ? '\0' : _expr.back();
    const bool operandEnd = isDigitChar(last) || last == ')' || last == '%';
    // The single ( ) key toggles: close the open group when it makes sense, otherwise open one
    // (with an implicit multiply after a number or a closed group).
    if (Calc::openParens(_expr) > 0 && operandEnd) {
        _expr += ')';
        return;
    }
    if (operandEnd) _expr += 'x';
    _expr += '(';
}

void ViewCalc::backspace() {
    _resultLine.clear();
    _showingResult = false;
    if (_expr == "Error") {
        _expr.clear();
        return;
    }
    if (!_expr.empty()) _expr.pop_back();
}

void ViewCalc::clearAll() {
    _expr.clear();
    _resultLine.clear();
    _showingResult = false;
}

void ViewCalc::equals() {
    if (_showingResult) return;
    if (!Calc::isComplete(_expr)) return; // half-typed expression: '=' does nothing

    // Auto-close the groups left open, like phone calculators do.
    int depth = Calc::openParens(_expr);
    while (depth-- > 0 && _expr.size() < MAX_EXPR) _expr += ')';

    const std::string shown = _expr;
    const Calc::EvalResult result = Calc::evaluate(_expr);
    _resultLine = shown;
    _expr = result.ok ? Calc::formatNumber(result.value) : "Error";
    _showingResult = true;
}

void ViewCalc::pressKey(int index) {
    const KeyDef& key = KEYS[index];
    switch (key.kind) {
        case KeyKind::Digit: appendDigit(key.label[0]); break;
        case KeyKind::Operator: appendOperator(key.label[0]); break;
        case KeyKind::Dot: appendDot(); break;
        case KeyKind::Percent: appendPercent(); break;
        case KeyKind::Paren: appendParen(); break;
        case KeyKind::Clear: clearAll(); break;
        case KeyKind::Backspace: backspace(); break;
        case KeyKind::Equals: equals(); break;
    }
}

// ---------------------------------------------------------------- touch
bool ViewCalc::handleTouch(const TouchEvent& ev) {
    // Click or Up: a tap with drift / longer than 1s arrives as Up (see TouchManager) and must work.
    if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return false;
    for (int i = 0; i < NUM_KEYS; ++i) {
        int x = 0, y = 0;
        keyRect(i, x, y);
        if (!inRect(ev, x, y, KEY_W, KEY_H)) continue;
        pressKey(i);
        refreshDisplay();
        return true;
    }
    return false;
}

} // namespace MonoMesh
