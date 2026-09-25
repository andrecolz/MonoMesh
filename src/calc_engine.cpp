#include "calc_engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace MonoMesh {
namespace Calc {

namespace {

// Recursive-descent parser over the display string. Every method sets _ok = false on the first
// syntax error; callers bail out as soon as the flag is down.
class Parser {
public:
    explicit Parser(const std::string& src) : _s(src) {}

    bool run(double& out) {
        _pos = 0;
        bool pct = false;
        const double v = parseExpr(pct);
        skipSpaces();
        if (!_ok || _pos != _s.size()) return false;
        out = v;
        return true;
    }

private:
    const std::string& _s;
    size_t _pos = 0;
    bool _ok = true;

    void skipSpaces() {
        while (_pos < _s.size() && _s[_pos] == ' ') ++_pos;
    }

    bool peek(char& c) {
        skipSpaces();
        if (_pos >= _s.size()) return false;
        c = _s[_pos];
        return true;
    }

    bool consume(char c) {
        char p;
        if (!peek(p) || p != c) return false;
        ++_pos;
        return true;
    }

    void fail() { _ok = false; }

    // expr := term (('+' | '-') term)*
    double parseExpr(bool& pct) {
        pct = false;
        double left = parseTerm(pct);
        char op;
        while (_ok && peek(op) && (op == '+' || op == '-')) {
            ++_pos;
            bool rightPct = false;
            const double right = parseTerm(rightPct);
            if (!_ok) return 0.0;
            // Relative percent: the term is already divided by 100, so a + b% is a + a*(b/100).
            const double rhs = rightPct ? left * right : right;
            left = (op == '+') ? left + rhs : left - rhs;
        }
        return left;
    }

    // term := factor (('x' | '/') factor)*
    double parseTerm(bool& pct) {
        bool firstPct = false;
        double v = parseFactor(firstPct);
        int factors = 1;
        char op;
        while (_ok && peek(op) && (op == 'x' || op == '/')) {
            ++_pos;
            bool factorPct = false;
            const double rhs = parseFactor(factorPct);
            if (!_ok) return 0.0;
            if (op == 'x') {
                v *= rhs;
            } else {
                if (rhs == 0.0) {
                    fail();
                    return 0.0;
                }
                v /= rhs;
            }
            ++factors;
        }
        // Only a term made of a single number carries the relative-percent meaning.
        pct = (factors == 1) && firstPct;
        return v;
    }

    // factor := unary ('%')*
    double parseFactor(bool& pct) {
        double v = parseUnary();
        pct = false;
        while (_ok && consume('%')) {
            v /= 100.0;
            pct = true;
        }
        return v;
    }

    // unary := '-' unary | '+' unary | primary
    double parseUnary() {
        char c;
        if (peek(c) && (c == '-' || c == '+')) {
            ++_pos;
            const double v = parseUnary();
            return (c == '-') ? -v : v;
        }
        return parsePrimary();
    }

    // primary := number | '(' expr ')'
    double parsePrimary() {
        char c;
        if (peek(c) && c == '(') {
            ++_pos;
            bool pct = false;
            const double v = parseExpr(pct);
            if (!_ok) return 0.0;
            if (!consume(')')) {
                fail();
                return 0.0;
            }
            return v;
        }
        skipSpaces();
        const size_t start = _pos;
        bool dot = false;
        while (_pos < _s.size()) {
            const char d = _s[_pos];
            if (d >= '0' && d <= '9') {
                ++_pos;
                continue;
            }
            if (d == '.' && !dot) {
                dot = true;
                ++_pos;
                continue;
            }
            break;
        }
        if (_pos == start) {
            fail();
            return 0.0;
        }
        const std::string num = _s.substr(start, _pos - start);
        return strtod(num.c_str(), nullptr);
    }
};

} // namespace

EvalResult evaluate(const std::string& expr) {
    if (expr.empty()) return {};
    double v = 0.0;
    Parser parser(expr);
    if (!parser.run(v)) return {};
    if (std::isnan(v) || std::isinf(v)) return {};
    EvalResult result;
    result.value = v;
    result.ok = true;
    return result;
}

int openParens(const std::string& expr) {
    int depth = 0;
    for (char c : expr) {
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        }
    }
    return depth;
}

bool isComplete(const std::string& expr) {
    if (expr.empty()) return false;
    if (openParens(expr) != 0) return false;
    const char last = expr.back();
    const bool operandEnd = (last >= '0' && last <= '9') || last == ')' || last == '%';
    if (!operandEnd) return false;
    for (char c : expr) {
        if (c >= '0' && c <= '9') return true;
    }
    return false;
}

std::string formatNumber(double value) {
    if (std::isnan(value) || std::isinf(value)) return "Error";
    if (value == 0.0) return "0";
    char buf[32];
    const double mag = std::fabs(value);
    if (mag >= 1e12 || mag < 1e-9) {
        snprintf(buf, sizeof(buf), "%.6e", value);
    } else {
        snprintf(buf, sizeof(buf), "%.10g", value);
    }
    return std::string(buf);
}

} // namespace Calc
} // namespace MonoMesh
