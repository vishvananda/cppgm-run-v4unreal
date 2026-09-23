#include "preprocess/expressions/ControlExpression.h"

#include <cstdint>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "preprocess/tokens/IPPTokenStream.h"
#include "preprocess/tokens/PPTokenizer.h"

namespace {

enum TokenKind
{
  TOKEN_IDENTIFIER,
  TOKEN_NUMBER,
  TOKEN_CHARACTER,
  TOKEN_OPERATOR,
  TOKEN_INVALID
};

struct Token
{
  TokenKind kind;
  std::string spelling;
  std::string original_spelling;
  bool identifier_spelling;
  Token(TokenKind k, const std::string & s, bool id = false,
        const std::string & original = std::string())
    : kind(k), spelling(s), original_spelling(original.empty() ? s : original),
      identifier_spelling(id) {}
};

struct Value
{
  uint64_t bits;
  bool is_unsigned;
  Value(uint64_t b = 0, bool u = false) : bits(b), is_unsigned(u) {}
};

class ExpressionFailure : public std::exception
{
public:
  const char * what() const throw() { return "invalid controlling expression"; }
};

int64_t signed_value(uint64_t bits)
{
  if (bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return static_cast<int64_t>(bits);
  // This form is defined even for the sign bit: -1 - INT64_MAX is INT64_MIN.
  return -1 - static_cast<int64_t>(~bits);
}

uint64_t unsigned_value(int64_t value)
{
  return static_cast<uint64_t>(value);
}

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(char c)
{
  return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
unsigned hex_digit_value(char c)
{
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
  return static_cast<unsigned>(c - 'A' + 10);
}

bool parse_integer_literal(const std::string & text, Value * result)
{
  if (text.empty()) return false;
  size_t at = 0;
  unsigned base = 10;
  size_t first_digit = 0;
  if (text.size() >= 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X'))
  {
    base = 16;
    at = 2;
    first_digit = at;
  }
  else if (text[0] == '0')
  {
    base = 8;
    first_digit = 0;
  }
  else
  {
    if (!is_ascii_digit(text[0])) return false;
    first_digit = 0;
  }

  uint64_t value = 0;
  size_t digits = 0;
  while (at < text.size())
  {
    unsigned digit;
    if (text[at] >= '0' && text[at] <= '9')
      digit = static_cast<unsigned>(text[at] - '0');
    else if (base == 16 && is_hex_digit(text[at]))
      digit = hex_digit_value(text[at]);
    else
      break;
    if (digit >= base) break;
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / base)
      return false;
    value = value * base + digit;
    ++at;
    ++digits;
  }
  if (digits == 0 || at == first_digit) return false;

  std::string suffix;
  bool unsigned_suffix = false;
  bool long_suffix = false;
  bool long_long_suffix = false;
  for (size_t i = at; i < text.size(); ++i)
  {
    char c = text[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    suffix.push_back(c);
  }
  if (suffix.empty())
  {
    // default suffix
  }
  else if (suffix == "u") unsigned_suffix = true;
  else if (suffix == "l") long_suffix = true;
  else if (suffix == "ll") long_long_suffix = true;
  else if (suffix == "ul" || suffix == "lu")
  {
    unsigned_suffix = true;
    long_suffix = true;
  }
  else if (suffix == "ull" || suffix == "llu")
  {
    unsigned_suffix = true;
    long_long_suffix = true;
  }
  else return false;

  // C++11 disallows mixed case within the two-letter long-long suffix.
  for (size_t i = at; i + 1 < text.size(); ++i)
  {
    if ((text[i] == 'l' || text[i] == 'L') &&
        (text[i + 1] == 'l' || text[i + 1] == 'L') &&
        text[i] != text[i + 1])
      return false;
  }

  const uint64_t int_max = 0x7fffffffULL;
  const uint64_t uint_max = 0xffffffffULL;
  const uint64_t long_max = 0x7fffffffffffffffULL;
  const uint64_t ulong_max = std::numeric_limits<uint64_t>::max();
  bool selected_unsigned = false;
  bool fits = false;

  if (unsigned_suffix)
  {
    if (long_long_suffix)
      fits = value <= ulong_max, selected_unsigned = true;
    else if (long_suffix)
      fits = value <= ulong_max, selected_unsigned = true;
    else
      fits = value <= ulong_max, selected_unsigned = true;
  }
  else if (long_long_suffix)
  {
    if (value <= long_max) fits = true;
    else if (base != 10 && value <= ulong_max)
      fits = true, selected_unsigned = true;
  }
  else if (long_suffix)
  {
    if (value <= long_max) fits = true;
    else if (base != 10 && value <= ulong_max)
      fits = true, selected_unsigned = true;
    else if (value <= long_max) fits = true;
    else if (base != 10 && value <= ulong_max)
      fits = true, selected_unsigned = true;
  }
  else if (base == 10)
  {
    if (value <= int_max || value <= long_max) fits = true;
  }
  else
  {
    if (value <= int_max) fits = true;
    else if (value <= uint_max) fits = true, selected_unsigned = true;
    else if (value <= long_max) fits = true;
    else fits = true, selected_unsigned = true;
  }

  if (!fits) return false;
  result->bits = value;
  result->is_unsigned = selected_unsigned;
  return true;
}

bool decode_utf8(const std::string & text, size_t at, uint32_t * cp, size_t * next)
{
  if (at >= text.size()) return false;
  const unsigned char first = static_cast<unsigned char>(text[at]);
  if (first < 0x80)
  {
    *cp = first;
    *next = at + 1;
    return true;
  }
  unsigned count;
  uint32_t value;
  if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 0x1f; }
  else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 0x0f; }
  else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 0x07; }
  else return false;
  if (at + count > text.size()) return false;
  for (unsigned i = 1; i < count; ++i)
  {
    const unsigned char c = static_cast<unsigned char>(text[at + i]);
    if ((c & 0xc0) != 0x80) return false;
    value = (value << 6) | (c & 0x3f);
  }
  if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
      (count == 4 && value < 0x10000) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff))
    return false;
  *cp = value;
  *next = at + count;
  return true;
}

bool parse_character_literal(const std::string & text, Value * result)
{
  if (text.size() < 3) return false;
  size_t quote = 0;
  enum Prefix { ORDINARY, UTF16, UTF32, WCHAR } prefix = ORDINARY;
  if (text[0] == 'u' || text[0] == 'U' || text[0] == 'L')
  {
    if (text.size() < 4 || text[1] != '\'') return false;
    quote = 1;
    prefix = text[0] == 'u' ? UTF16 : (text[0] == 'U' ? UTF32 : WCHAR);
  }
  else if (text[0] != '\'') return false;
  if (text[quote] != '\'' || text[text.size() - 1] != '\'') return false;
  const size_t begin = quote + 1;
  const size_t end = text.size() - 1;
  if (begin >= end) return false;

  uint32_t value = 0;
  if (text[begin] != '\\')
  {
    size_t next;
    if (!decode_utf8(text, begin, &value, &next) || next != end) return false;
  }
  else
  {
    size_t i = begin + 1;
    if (i >= end) return false;
    const char c = text[i++];
    switch (c)
    {
      case '\'': value = '\''; break;
      case '"': value = '"'; break;
      case '?': value = '?'; break;
      case '\\': value = '\\'; break;
      case 'a': value = 7; break;
      case 'b': value = 8; break;
      case 'f': value = 12; break;
      case 'n': value = 10; break;
      case 'r': value = 13; break;
      case 't': value = 9; break;
      case 'v': value = 11; break;
      default:
        if (c == 'x')
        {
          if (i == end || !is_hex_digit(text[i])) return false;
          uint64_t parsed = 0;
          for (; i < end && is_hex_digit(text[i]); ++i)
          {
            const unsigned digit = hex_digit_value(text[i]);
            if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 16)
              return false;
            parsed = parsed * 16 + digit;
          }
          value = static_cast<uint32_t>(parsed);
        }
        else if (c >= '0' && c <= '7')
        {
          unsigned parsed = static_cast<unsigned>(c - '0');
          unsigned count = 1;
          while (i < end && count < 3 && text[i] >= '0' && text[i] <= '7')
          {
            parsed = parsed * 8 + static_cast<unsigned>(text[i++] - '0');
            ++count;
          }
          value = parsed;
        }
        else return false;
        break;
    }
    if (i != end) return false;
  }

  if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  if (prefix == UTF16 && value > 0xffff) return false;
  result->bits = value;
  result->is_unsigned = prefix == UTF16 || prefix == UTF32;
  // Ordinary non-ASCII character literals have type int; wchar_t is signed
  // on the course Linux x86-64 ABI.
  return true;
}

bool is_alternative_identifier(const std::string & s)
{
  // PA1 emits these two C++ keywords through the operator callback too;
  // unlike the alternative operator names, they remain primary identifiers
  // in the PA3 controlling-expression grammar.
  return s == "new" || s == "delete" || s == "and" || s == "or" ||
         s == "not" || s == "bitand" ||
         s == "bitor" || s == "xor" || s == "compl" || s == "and_eq" ||
         s == "or_eq" || s == "xor_eq" || s == "not_eq";
}

bool is_identifier_operand(const Token & token)
{
  return token.identifier_spelling;
}

std::string canonical_operator(const std::string & source)
{
  if (source == "and") return "&&";
  if (source == "or") return "||";
  if (source == "not") return "!";
  if (source == "bitand") return "&";
  if (source == "bitor") return "|";
  if (source == "xor") return "^";
  if (source == "compl") return "~";
  if (source == "not_eq") return "!=";
  if (source == "and_eq") return "&=";
  if (source == "or_eq") return "|=";
  if (source == "xor_eq") return "^=";
  if (source == "<:") return "[";
  if (source == ":>") return "]";
  if (source == "<%") return "{";
  if (source == "%>") return "}";
  return source;
}

class Parser
{
public:
  explicit Parser(const std::vector<Token> & tokens)
    : tokens_(tokens), at_(0), recursion_depth_(0) {}

  Value parse()
  {
    Value value = parse_conditional(true);
    if (at_ != tokens_.size()) throw ExpressionFailure();
    return value;
  }

private:
  const std::vector<Token> & tokens_;
  size_t at_;
  unsigned recursion_depth_;

  const Token * peek() const
  {
    return at_ < tokens_.size() ? &tokens_[at_] : NULL;
  }

  bool consume(const char * op)
  {
    if (at_ < tokens_.size() && tokens_[at_].kind == TOKEN_OPERATOR &&
        tokens_[at_].spelling == op)
    {
      ++at_;
      return true;
    }
    return false;
  }

  const Token & take()
  {
    if (at_ >= tokens_.size()) throw ExpressionFailure();
    return tokens_[at_++];
  }

  bool at_operator(const char * op) const
  {
    return at_ < tokens_.size() && tokens_[at_].kind == TOKEN_OPERATOR &&
           tokens_[at_].spelling == op;
  }

  void enter_recursion()
  {
    // Keep malformed, adversarially deep input from exhausting the process
    // stack. This does not affect any bounded-work ordinary expression.
    if (++recursion_depth_ > 2048) throw ExpressionFailure();
  }
  void leave_recursion() { --recursion_depth_; }

  static bool truth(const Value & value) { return value.bits != 0; }
  static Value boolean(bool value) { return Value(value ? 1 : 0, false); }

  Value parse_primary(bool evaluate)
  {
    if (consume("("))
    {
      enter_recursion();
      Value nested;
      try
      {
        nested = parse_conditional(evaluate);
        if (!consume(")")) throw ExpressionFailure();
      }
      catch (...)
      {
        leave_recursion();
        throw;
      }
      leave_recursion();
      return nested;
    }

    const Token & token = take();
    if (token.kind == TOKEN_NUMBER)
    {
      Value value;
      if (!parse_integer_literal(token.spelling, &value)) throw ExpressionFailure();
      return value;
    }
    if (token.kind == TOKEN_CHARACTER)
    {
      Value value;
      if (!parse_character_literal(token.spelling, &value)) throw ExpressionFailure();
      return value;
    }
    if (token.kind == TOKEN_OPERATOR && token.identifier_spelling &&
        (token.original_spelling == "new" || token.original_spelling == "delete"))
      return Value(0, false);
    if (token.kind == TOKEN_IDENTIFIER)
    {
      if (token.spelling == "true") return Value(1, false);
      if (token.spelling == "false") return Value(0, false);
      if (token.spelling != "defined") return Value(0, false);

      bool parenthesized = consume("(");
      if (at_ >= tokens_.size() || !is_identifier_operand(tokens_[at_]))
        throw ExpressionFailure();
      const std::string identifier = tokens_[at_++].original_spelling;
      if (parenthesized && !consume(")")) throw ExpressionFailure();
      const unsigned char first = identifier.empty() ? 0 :
        static_cast<unsigned char>(identifier[0]);
      return Value((first & 1u) ? 1 : 0, false);
    }
    throw ExpressionFailure();
  }

  Value parse_unary(bool evaluate)
  {
    const char * unary = NULL;
    if (at_operator("+")) unary = "+";
    else if (at_operator("-")) unary = "-";
    else if (at_operator("!")) unary = "!";
    else if (at_operator("~")) unary = "~";
    if (unary == NULL) return parse_primary(evaluate);

    const std::string op(unary);
    ++at_;
    enter_recursion();
    Value operand;
    try { operand = parse_unary(evaluate); }
    catch (...) { leave_recursion(); throw; }
    leave_recursion();
    if (!evaluate) return op == "!" ? boolean(false) : operand;
    if (op == "+") return operand;
    if (op == "-") return Value(uint64_t(0) - operand.bits, operand.is_unsigned);
    if (op == "~") return Value(~operand.bits, operand.is_unsigned);
    return boolean(!truth(operand));
  }

  Value apply(const std::string & op, const Value & lhs, const Value & rhs,
              bool evaluate)
  {
    const bool common_unsigned = lhs.is_unsigned || rhs.is_unsigned;
    if (op == "&&" || op == "||")
    {
      if (!evaluate) return boolean(false);
      return boolean(op == "&&" ? truth(lhs) && truth(rhs)
                                 : truth(lhs) || truth(rhs));
    }
    if (op == "==" || op == "!=" || op == "<" || op == ">" ||
        op == "<=" || op == ">=")
    {
      if (!evaluate) return boolean(false);
      bool result = false;
      if (common_unsigned)
      {
        if (op == "==") result = lhs.bits == rhs.bits;
        else if (op == "!=") result = lhs.bits != rhs.bits;
        else if (op == "<") result = lhs.bits < rhs.bits;
        else if (op == ">") result = lhs.bits > rhs.bits;
        else if (op == "<=") result = lhs.bits <= rhs.bits;
        else result = lhs.bits >= rhs.bits;
      }
      else
      {
        const int64_t a = signed_value(lhs.bits), b = signed_value(rhs.bits);
        if (op == "==") result = a == b;
        else if (op == "!=") result = a != b;
        else if (op == "<") result = a < b;
        else if (op == ">") result = a > b;
        else if (op == "<=") result = a <= b;
        else result = a >= b;
      }
      return boolean(result);
    }

    if (op == "<<" || op == ">>")
    {
      if (!evaluate) return Value(0, lhs.is_unsigned);
      uint64_t amount;
      if (rhs.is_unsigned)
      {
        if (rhs.bits >= 64) throw ExpressionFailure();
        amount = rhs.bits;
      }
      else
      {
        const int64_t signed_amount = signed_value(rhs.bits);
        if (signed_amount < 0 || signed_amount >= 64) throw ExpressionFailure();
        amount = static_cast<uint64_t>(signed_amount);
      }
      if (op == "<<") return Value(lhs.bits << amount, lhs.is_unsigned);
      if (amount == 0) return lhs;
      uint64_t shifted = lhs.bits >> amount;
      if (!lhs.is_unsigned && (lhs.bits >> 63) != 0)
        shifted |= (~uint64_t(0)) << (64 - amount);
      return Value(shifted, lhs.is_unsigned);
    }

    if (op == "/" || op == "%")
    {
      if (!evaluate) return Value(0, common_unsigned);
      if (common_unsigned)
      {
        if (rhs.bits == 0) throw ExpressionFailure();
        return Value(op == "/" ? lhs.bits / rhs.bits : lhs.bits % rhs.bits, true);
      }
      const int64_t a = signed_value(lhs.bits), b = signed_value(rhs.bits);
      if (b == 0 || (a == std::numeric_limits<int64_t>::min() && b == -1))
        throw ExpressionFailure();
      return Value(unsigned_value(op == "/" ? a / b : a % b), false);
    }

    if (!evaluate) return Value(0, common_unsigned);
    if (op == "*") return Value(lhs.bits * rhs.bits, common_unsigned);
    if (op == "+") return Value(lhs.bits + rhs.bits, common_unsigned);
    if (op == "-") return Value(lhs.bits - rhs.bits, common_unsigned);
    if (op == "&") return Value(lhs.bits & rhs.bits, common_unsigned);
    if (op == "^") return Value(lhs.bits ^ rhs.bits, common_unsigned);
    if (op == "|") return Value(lhs.bits | rhs.bits, common_unsigned);
    throw ExpressionFailure();
  }

  Value parse_multiplicative(bool evaluate)
  {
    Value value = parse_unary(evaluate);
    while (at_operator("*") || at_operator("/") || at_operator("%"))
    {
      const std::string op = take().spelling;
      const Value rhs = parse_unary(evaluate);
      value = apply(op, value, rhs, evaluate);
    }
    return value;
  }

  Value parse_additive(bool evaluate)
  {
    Value value = parse_multiplicative(evaluate);
    while (at_operator("+") || at_operator("-"))
    {
      const std::string op = take().spelling;
      const Value rhs = parse_multiplicative(evaluate);
      value = apply(op, value, rhs, evaluate);
    }
    return value;
  }

  Value parse_shift(bool evaluate)
  {
    Value value = parse_additive(evaluate);
    while (at_operator("<<") || at_operator(">>"))
    {
      const std::string op = take().spelling;
      const Value rhs = parse_additive(evaluate);
      value = apply(op, value, rhs, evaluate);
    }
    return value;
  }

  Value parse_relational(bool evaluate)
  {
    Value value = parse_shift(evaluate);
    while (at_operator("<") || at_operator(">") || at_operator("<=") ||
           at_operator(">="))
    {
      const std::string op = take().spelling;
      const Value rhs = parse_shift(evaluate);
      value = apply(op, value, rhs, evaluate);
    }
    return value;
  }

  Value parse_equality(bool evaluate)
  {
    Value value = parse_relational(evaluate);
    while (at_operator("==") || at_operator("!="))
    {
      const std::string op = take().spelling;
      const Value rhs = parse_relational(evaluate);
      value = apply(op, value, rhs, evaluate);
    }
    return value;
  }

  Value parse_and(bool evaluate)
  {
    Value value = parse_equality(evaluate);
    while (consume("&"))
    {
      const Value rhs = parse_equality(evaluate);
      value = apply("&", value, rhs, evaluate);
    }
    return value;
  }

  Value parse_xor(bool evaluate)
  {
    Value value = parse_and(evaluate);
    while (consume("^"))
    {
      const Value rhs = parse_and(evaluate);
      value = apply("^", value, rhs, evaluate);
    }
    return value;
  }

  Value parse_or(bool evaluate)
  {
    Value value = parse_xor(evaluate);
    while (consume("|"))
    {
      const Value rhs = parse_xor(evaluate);
      value = apply("|", value, rhs, evaluate);
    }
    return value;
  }

  Value parse_logical_and(bool evaluate)
  {
    Value value = parse_or(evaluate);
    while (consume("&&"))
    {
      const bool evaluate_rhs = evaluate && truth(value);
      const Value rhs = parse_or(evaluate_rhs);
      value = apply("&&", value, rhs, evaluate);
    }
    return value;
  }

  Value parse_logical_or(bool evaluate)
  {
    Value value = parse_logical_and(evaluate);
    while (consume("||"))
    {
      const bool evaluate_rhs = evaluate && !truth(value);
      const Value rhs = parse_logical_and(evaluate_rhs);
      value = apply("||", value, rhs, evaluate);
    }
    return value;
  }

  Value parse_conditional(bool evaluate)
  {
    Value condition = parse_logical_or(evaluate);
    if (!consume("?")) return condition;

    const bool select_true = evaluate && truth(condition);
    enter_recursion();
    Value when_true;
    try
    {
      when_true = parse_conditional(select_true);
      if (!consume(":")) throw ExpressionFailure();
    }
    catch (...)
    {
      leave_recursion();
      throw;
    }
    const bool select_false = evaluate && !truth(condition);
    Value when_false;
    try { when_false = parse_conditional(select_false); }
    catch (...) { leave_recursion(); throw; }
    leave_recursion();

    const bool common_unsigned = when_true.is_unsigned || when_false.is_unsigned;
    if (!evaluate) return Value(0, common_unsigned);
    const Value selected = select_true ? when_true : when_false;
    return Value(selected.bits, common_unsigned);
  }
};

void emit_error(std::ostream & output) { output << "error\n"; }

void emit_value(std::ostream & output, const Value & value)
{
  if (value.is_unsigned)
    output << value.bits << "u\n";
  else
    output << signed_value(value.bits) << "\n";
}

class ControllingExpressionStream : public IPPTokenStream
{
public:
  explicit ControllingExpressionStream(std::ostream & output) : output_(output) {}

  void emit_whitespace_sequence() {}
  void emit_new_line() { finish_line(); }
  void emit_header_name(const std::string & data) { add(TOKEN_INVALID, data); }
  void emit_identifier(const std::string & data) { add(TOKEN_IDENTIFIER, data, true); }
  void emit_pp_number(const std::string & data) { add(TOKEN_NUMBER, data); }
  void emit_character_literal(const std::string & data) { add(TOKEN_CHARACTER, data); }
  void emit_user_defined_character_literal(const std::string & data)
    { add(TOKEN_INVALID, data); }
  void emit_string_literal(const std::string & data) { add(TOKEN_INVALID, data); }
  void emit_user_defined_string_literal(const std::string & data)
    { add(TOKEN_INVALID, data); }
  void emit_preprocessing_op_or_punc(const std::string & data)
    { add(TOKEN_OPERATOR, canonical_operator(data), is_alternative_identifier(data), data); }
  void emit_non_whitespace_char(const std::string & data) { add(TOKEN_INVALID, data); }
  void emit_eof()
  {
    finish_line();
    output_ << "eof\n";
  }

private:
  std::ostream & output_;
  std::vector<Token> line_;

  void add(TokenKind kind, const std::string & spelling, bool identifier = false,
           const std::string & original = std::string())
  {
    line_.push_back(Token(kind, spelling, identifier, original));
  }

  void finish_line()
  {
    if (line_.empty()) return;
    try
    {
      Parser parser(line_);
      emit_value(output_, parser.parse());
    }
    catch (const ExpressionFailure &)
    {
      emit_error(output_);
    }
    line_.clear();
  }
};

} // namespace

void run_control_expression_tool(const std::string & source, std::ostream & output)
{
  ControllingExpressionStream stream(output);
  PPTokenizer tokenizer(source, stream);
  tokenizer.tokenize();
}
