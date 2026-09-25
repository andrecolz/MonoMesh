#pragma once

#include <string>

namespace MonoMesh {
namespace Calc {

// Result of evaluating a user-typed expression.
struct EvalResult {
    double value = 0.0;
    bool ok = false;
};

// Evaluates an expression typed on the CALC keypad. The string uses the same characters shown on
// the display: digits, '.', 'x' for multiply, '/', '+', '-', '%' and parentheses. Grammar
// (recursive descent):
//   expr    := term (('+' | '-') term)*
//   term    := factor (('x' | '/') factor)*
//   factor  := unary ('%')*
//   unary   := '-' unary | '+' unary | primary
//   primary := number | '(' expr ')'
// Percent is contextual like a phone calculator: "a + b%" is a + a*b/100, "a - b%" is a - a*b/100,
// everywhere else it is a plain /100. Division by zero, bad syntax and NaN/Inf fail (ok = false).
EvalResult evaluate(const std::string& expr);

// True when the expression ends on a complete operand with balanced parentheses: used by '=' to
// ignore a half-typed expression instead of showing an error.
bool isComplete(const std::string& expr);

// Number of '(' still open (negative means extra ')').
int openParens(const std::string& expr);

// Display form of a result: up to 10 significant digits, no trailing zeros, exponent notation for
// very large/small magnitudes, "Error" for NaN/Inf.
std::string formatNumber(double value);

} // namespace Calc
} // namespace MonoMesh
