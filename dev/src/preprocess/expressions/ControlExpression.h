#pragma once

#include <iosfwd>
#include <string>
#include <vector>

// Run PA3's controlling-expression tool over a UTF-8 source buffer. Phase 1-3
// failures are reported by PPTokenizer as exceptions; expression failures are
// isolated to their logical source line.
void run_control_expression_tool(const std::string & source, std::ostream & output);

struct ControlExpressionToken
{
  enum Kind { IDENTIFIER, PP_NUMBER, CHARACTER_LITERAL, OPERATOR, INVALID } kind;
  std::string spelling;
  ControlExpressionToken(Kind k, const std::string & s)
    : kind(k), spelling(s) {}
};

// Evaluate already macro-expanded, typed preprocessing tokens. Remaining
// identifiers must already have been replaced with integer zero by the caller.
bool evaluate_control_expression_tokens(
  const std::vector<ControlExpressionToken> & tokens, bool * result);
