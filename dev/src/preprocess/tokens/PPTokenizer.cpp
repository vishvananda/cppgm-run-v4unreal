#include "preprocess/tokens/PPTokenizer.h"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

const int kEof = -1;

struct Unit
{
  int cp;
  std::size_t begin;
  std::size_t end;
  std::size_t line;
  std::size_t column;

  Unit(int value = kEof, std::size_t first = 0, std::size_t last = 0,
       std::size_t source_line = 1, std::size_t source_column = 1)
    : cp(value), begin(first), end(last), line(source_line),
      column(source_column)
  {}
};

bool decode_utf8(const std::string & source, std::size_t offset,
                 int * code_point, std::size_t * next)
{
  if (offset >= source.size())
    return false;
  const unsigned char first = static_cast<unsigned char>(source[offset]);
  unsigned int value = 0;
  std::size_t count = 0;
  if (first <= 0x7f)
  {
    value = first;
    count = 1;
  }
  else if (first >= 0xc2 && first <= 0xdf)
  {
    value = first & 0x1f;
    count = 2;
  }
  else if (first >= 0xe0 && first <= 0xef)
  {
    value = first & 0x0f;
    count = 3;
  }
  else if (first >= 0xf0 && first <= 0xf4)
  {
    value = first & 0x07;
    count = 4;
  }
  else
  {
    throw std::runtime_error("invalid UTF-8 source character");
  }
  if (offset + count > source.size())
    throw std::runtime_error("incomplete UTF-8 source character");
  for (std::size_t i = 1; i < count; ++i)
  {
    const unsigned char continuation =
      static_cast<unsigned char>(source[offset + i]);
    if ((continuation & 0xc0) != 0x80)
      throw std::runtime_error("invalid UTF-8 continuation byte");
    if (i == 1 && ((first == 0xe0 && continuation < 0xa0) ||
                   (first == 0xed && continuation >= 0xa0) ||
                   (first == 0xf0 && continuation < 0x90) ||
                   (first == 0xf4 && continuation >= 0x90)))
      throw std::runtime_error("invalid UTF-8 code point");
    value = (value << 6) | (continuation & 0x3f);
  }
  if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
    throw std::runtime_error("invalid UTF-8 code point");
  *code_point = static_cast<int>(value);
  *next = offset + count;
  return true;
}

void append_utf8(int cp, std::string * result)
{
  if (cp < 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    throw std::runtime_error("invalid universal character value");
  if (cp <= 0x7f)
    result->push_back(static_cast<char>(cp));
  else if (cp <= 0x7ff)
  {
    result->push_back(static_cast<char>(0xc0 | (cp >> 6)));
    result->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
  else if (cp <= 0xffff)
  {
    result->push_back(static_cast<char>(0xe0 | (cp >> 12)));
    result->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    result->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
  else
  {
    result->push_back(static_cast<char>(0xf0 | (cp >> 18)));
    result->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    result->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    result->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

int hex_value(int cp)
{
  if (cp >= '0' && cp <= '9') return cp - '0';
  if (cp >= 'a' && cp <= 'f') return cp - 'a' + 10;
  if (cp >= 'A' && cp <= 'F') return cp - 'A' + 10;
  return -1;
}

// Phase 1 maps UTF-8 and trigraphs, and phase 2 removes backslash/newline
// pairs.  The bounded queues are only lookahead for those two transformations.
class Phase12Cursor
{
public:
  explicit Phase12Cursor(const std::string & source)
    : source_(source), byte_(0), line_(1), column_(1), phase1_eof_(false),
      has_pending_phase1_(false), synthetic_emitted_(false), finished_(false)
  {
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xef &&
        static_cast<unsigned char>(source_[1]) == 0xbb &&
        static_cast<unsigned char>(source_[2]) == 0xbf)
      byte_ = 3; // A UTF-8 signature is not a source token.
  }

  Unit next()
  {
    if (finished_)
      return Unit();
    for (;;)
    {
      Unit current;
      if (has_pending_phase1_)
      {
        current = pending_phase1_;
        has_pending_phase1_ = false;
      }
      else
        current = read_phase1();

      if (current.cp == kEof)
      {
        if (!synthetic_emitted_ && needs_appended_newline())
        {
          synthetic_emitted_ = true;
          return Unit('\n', source_.size(), source_.size(), line_, column_);
        }
        finished_ = true;
        return Unit();
      }

      if (current.cp == '\\')
      {
        Unit following = read_phase1();
        // Phase 2 sees the phase-2-mandated final newline. It must therefore
        // splice a final backslash just as it would any other backslash-newline
        // pair; appending only after tokenization loses this distinction.
        if (following.cp == kEof && !synthetic_emitted_ &&
            needs_appended_newline())
        {
          synthetic_emitted_ = true;
          following = Unit('\n', source_.size(), source_.size(), line_, column_);
        }
        if (following.cp == '\n')
          continue;
        if (following.cp != kEof)
        {
          pending_phase1_ = following;
          has_pending_phase1_ = true;
        }
      }
      return current;
    }
  }

  void reset_after_raw_literal(std::size_t byte, std::size_t line,
                               std::size_t column)
  {
    byte_ = byte;
    line_ = line;
    column_ = column;
    phase1_lookahead_.clear();
    phase1_eof_ = false;
    has_pending_phase1_ = false;
    synthetic_emitted_ = false;
    finished_ = false;
  }

private:
  const std::string & source_;
  std::size_t byte_;
  std::size_t line_;
  std::size_t column_;
  std::deque<Unit> phase1_lookahead_;
  bool phase1_eof_;
  Unit pending_phase1_;
  bool has_pending_phase1_;
  bool synthetic_emitted_;
  bool finished_;

  bool needs_appended_newline() const
  {
    if (source_.empty())
      return false;
    if (source_[source_.size() - 1] != '\n')
      return true;
    // A final backslash-newline is removed in phase 2, so phase 2 first
    // appends another newline. Account for a phase-1 trigraph spelling too.
    if (source_.size() >= 2 && source_[source_.size() - 2] == '\\')
      return true;
    return source_.size() >= 4 &&
           source_.compare(source_.size() - 4, 3, "?" "?/") == 0;
  }

  Unit decode_physical()
  {
    if (byte_ >= source_.size())
      return Unit();
    int cp;
    std::size_t next_byte;
    if (!decode_utf8(source_, byte_, &cp, &next_byte))
      return Unit();
    Unit unit(cp, byte_, next_byte, line_, column_);
    byte_ = next_byte;
    if (cp == '\n')
    {
      ++line_;
      column_ = 1;
    }
    else
      ++column_;
    return unit;
  }

  void ensure_phase1(std::size_t count)
  {
    while (phase1_lookahead_.size() < count && !phase1_eof_)
    {
      Unit unit = decode_physical();
      if (unit.cp == kEof)
        phase1_eof_ = true;
      else
        phase1_lookahead_.push_back(unit);
    }
  }

  Unit read_phase1()
  {
    ensure_phase1(1);
    if (phase1_lookahead_.empty())
      return Unit();
    ensure_phase1(3);
    Unit first = phase1_lookahead_[0];
    if (first.cp == '?' && phase1_lookahead_.size() >= 3 &&
        phase1_lookahead_[1].cp == '?')
    {
      const int third = phase1_lookahead_[2].cp;
      int replacement = kEof;
      switch (third)
      {
      case '=': replacement = '#'; break;
      case '/': replacement = '\\'; break;
      case '\'': replacement = '^'; break;
      case '(': replacement = '['; break;
      case ')': replacement = ']'; break;
      case '!': replacement = '|'; break;
      case '<': replacement = '{'; break;
      case '>': replacement = '}'; break;
      case '-': replacement = '~'; break;
      default: break;
      }
      if (replacement != kEof)
      {
        Unit mapped(replacement, first.begin, phase1_lookahead_[2].end,
                    first.line, first.column);
        phase1_lookahead_.pop_front();
        phase1_lookahead_.pop_front();
        phase1_lookahead_.pop_front();
        return mapped;
      }
    }
    phase1_lookahead_.pop_front();
    return first;
  }
};

bool range_contains(const std::vector<std::pair<int, int> > & ranges, int cp)
{
  std::size_t low = 0;
  std::size_t high = ranges.size();
  while (low < high)
  {
    const std::size_t middle = low + (high - low) / 2;
    if (cp < ranges[middle].first)
      high = middle;
    else if (cp > ranges[middle].second)
      low = middle + 1;
    else
      return true;
  }
  return false;
}

const std::vector<std::pair<int, int> > & annex_e1()
{
  // N3485 Annex E.1: immutable table, binary-searched by code point.
  static const std::vector<std::pair<int, int> > ranges = {
    {0xA8,0xA8},{0xAA,0xAA},{0xAD,0xAD},{0xAF,0xAF},
    {0xB2,0xB5},{0xB7,0xBA},{0xBC,0xBE},{0xC0,0xD6},
    {0xD8,0xF6},{0xF8,0xFF},{0x100,0x167F},{0x1681,0x180D},
    {0x180F,0x1FFF},{0x200B,0x200D},{0x202A,0x202E},
    {0x203F,0x2040},{0x2054,0x2054},{0x2060,0x206F},
    {0x2070,0x218F},{0x2460,0x24FF},{0x2776,0x2793},
    {0x2C00,0x2DFF},{0x2E80,0x2FFF},{0x3004,0x3007},
    {0x3021,0x302F},{0x3031,0x303F},{0x3040,0xD7FF},
    {0xF900,0xFD3D},{0xFD40,0xFDCF},{0xFDF0,0xFE44},
    {0xFE47,0xFFFD},{0x10000,0x1FFFD},{0x20000,0x2FFFD},
    {0x30000,0x3FFFD},{0x40000,0x4FFFD},{0x50000,0x5FFFD},
    {0x60000,0x6FFFD},{0x70000,0x7FFFD},{0x80000,0x8FFFD},
    {0x90000,0x9FFFD},{0xA0000,0xAFFFD},{0xB0000,0xBFFFD},
    {0xC0000,0xCFFFD},{0xD0000,0xDFFFD},{0xE0000,0xEFFFD}
  };
  return ranges;
}

const std::vector<std::pair<int, int> > & annex_e2()
{
  // N3485 Annex E.2: these code points may continue, but not start, names.
  static const std::vector<std::pair<int, int> > ranges = {
    {0x300,0x36F},{0x1DC0,0x1DFF},{0x20D0,0x20FF},{0xFE20,0xFE2F}
  };
  return ranges;
}

bool is_ascii_nondigit(int cp)
{
  return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
}

bool is_digit(int cp) { return cp >= '0' && cp <= '9'; }

bool is_identifier_start(int cp)
{
  if (is_ascii_nondigit(cp))
    return true;
  return range_contains(annex_e1(), cp) && !range_contains(annex_e2(), cp);
}

bool is_identifier_continue(int cp)
{
  return is_ascii_nondigit(cp) || is_digit(cp) ||
         range_contains(annex_e1(), cp);
}

bool is_space_without_newline(int cp)
{
  return cp == ' ' || cp == '\t' || cp == '\v' || cp == '\f';
}

bool is_hex(int cp) { return hex_value(cp) >= 0; }
bool is_octal(int cp) { return cp >= '0' && cp <= '7'; }

struct Ucn
{
  bool present;
  std::size_t length;
  int value;
  Ucn() : present(false), length(0), value(0) {}
};

struct StringStart
{
  bool present;
  bool raw;
  std::size_t prefix_length;
  char quote;
  StringStart() : present(false), raw(false), prefix_length(0), quote('"') {}
};

class Lexer
{
public:
  Lexer(const std::string & source, IPPTokenStream & output)
    : source_(source), output_(output), cursor_(source), directive_(0),
      line_start_(true)
  {}

  void run()
  {
    for (;;)
    {
      const int cp = peek(0).cp;
      if (cp == kEof)
        break;
      if (cp == '\n')
      {
        const Unit newline = take();
        output_.set_source_location(newline.line, newline.column);
        output_.emit_new_line();
        line_start_ = true;
        directive_ = 0;
        continue;
      }
      if (is_space_without_newline(cp) || is_comment_start())
      {
        scan_whitespace();
        continue;
      }

      const Unit start = peek(0);
      if (directive_ == 2 && (cp == '<' || cp == '"'))
      {
        const std::string header = scan_header_name();
        output_.set_source_location(start.line, start.column);
        output_.emit_header_name(header);
        finish_token(3, false, std::string());
        continue;
      }

      StringStart string_start = detect_string_start();
      if (string_start.present)
      {
        std::string spelling = string_start.raw
          ? scan_raw_string(string_start)
          : scan_quoted_literal(string_start);
        std::string suffix;
        const bool user_defined = scan_ud_suffix(&suffix);
        spelling += suffix;
        output_.set_source_location(start.line, start.column);
        if (string_start.quote == '\'')
        {
          if (user_defined)
            output_.emit_user_defined_character_literal(spelling);
          else
            output_.emit_character_literal(spelling);
        }
        else
        {
          if (user_defined)
            output_.emit_user_defined_string_literal(spelling);
          else
            output_.emit_string_literal(spelling);
        }
        finish_token(0, false, std::string());
        continue;
      }

      int logical_cp;
      std::size_t logical_length;
      logical_peek(0, &logical_cp, &logical_length);
      if (is_identifier_start(logical_cp))
      {
        std::string identifier = scan_identifier();
        static const char * alternative_ops[] = {
          "new", "delete", "and", "and_eq", "bitand", "bitor",
          "compl", "not", "not_eq", "or", "or_eq", "xor", "xor_eq"
        };
        bool alternative = false;
        for (std::size_t i = 0;
             i < sizeof(alternative_ops) / sizeof(alternative_ops[0]); ++i)
          if (identifier == alternative_ops[i])
          {
            alternative = true;
            break;
          }
        output_.set_source_location(start.line, start.column);
        if (alternative)
          output_.emit_preprocessing_op_or_punc(identifier);
        else
          output_.emit_identifier(identifier);
        finish_token(1, !alternative, identifier);
        continue;
      }

      if (is_digit(cp) || (cp == '.' && is_digit(peek(1).cp)))
      {
        const std::string number = scan_pp_number();
        output_.set_source_location(start.line, start.column);
        output_.emit_pp_number(number);
        finish_token(0, false, std::string());
        continue;
      }

      std::string op;
      if (scan_operator(&op))
      {
        output_.set_source_location(start.line, start.column);
        output_.emit_preprocessing_op_or_punc(op);
        finish_token(0, false, op);
        continue;
      }

      const int data_cp = take_logical();
      if (data_cp == '\'' || data_cp == '"')
        throw std::runtime_error("unterminated quoted preprocessing token");
      std::string data;
      append_utf8(data_cp, &data);
      output_.set_source_location(start.line, start.column);
      output_.emit_non_whitespace_char(data);
      finish_token(0, false, std::string());
    }
    output_.emit_eof();
  }

private:
  const std::string & source_;
  IPPTokenStream & output_;
  Phase12Cursor cursor_;
  std::deque<Unit> lookahead_;
  int directive_; // 0: none, 1: after #, 2: expecting header name
  bool line_start_;

  Unit peek(std::size_t index)
  {
    while (lookahead_.size() <= index &&
           (lookahead_.empty() || lookahead_.back().cp != kEof))
      lookahead_.push_back(cursor_.next());
    if (index < lookahead_.size())
      return lookahead_[index];
    return Unit();
  }

  Unit take()
  {
    Unit unit = peek(0);
    if (unit.cp != kEof)
      lookahead_.pop_front();
    return unit;
  }

  Ucn inspect_ucn(std::size_t index)
  {
    Ucn result;
    if (peek(index).cp != '\\')
      return result;
    const int marker = peek(index + 1).cp;
    if (marker != 'u' && marker != 'U')
      return result;
    const std::size_t digits = marker == 'u' ? 4 : 8;
    unsigned int value = 0;
    for (std::size_t i = 0; i < digits; ++i)
    {
      const int digit = hex_value(peek(index + 2 + i).cp);
      if (digit < 0)
        return result;
      value = (value << 4) | static_cast<unsigned int>(digit);
    }
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
      throw std::runtime_error("invalid universal character value");
    result.present = true;
    result.length = digits + 2;
    result.value = static_cast<int>(value);
    return result;
  }

  void logical_peek(std::size_t index, int * cp, std::size_t * length)
  {
    Ucn ucn = inspect_ucn(index);
    if (ucn.present)
    {
      *cp = ucn.value;
      *length = ucn.length;
    }
    else
    {
      *cp = peek(index).cp;
      *length = 1;
    }
  }

  int take_logical()
  {
    Ucn ucn = inspect_ucn(0);
    if (ucn.present)
    {
      for (std::size_t i = 0; i < ucn.length; ++i)
        take();
      return ucn.value;
    }
    return take().cp;
  }

  void append_logical(std::string * spelling)
  {
    append_utf8(take_logical(), spelling);
  }

  bool is_comment_start()
  {
    return peek(0).cp == '/' &&
           (peek(1).cp == '/' || peek(1).cp == '*');
  }

  void emit_whitespace(const Unit & start)
  {
    output_.set_source_location(start.line, start.column);
    output_.emit_whitespace_sequence();
  }

  void emit_comment_newline(const Unit & newline)
  {
    output_.set_source_location(newline.line, newline.column);
    output_.emit_new_line();
    line_start_ = true;
    directive_ = 0;
  }

  void scan_whitespace()
  {
    bool pending = false;
    bool have_start = false;
    Unit start;
    for (;;)
    {
      const int cp = peek(0).cp;
      if (is_space_without_newline(cp))
      {
        if (!have_start)
        {
          start = peek(0);
          have_start = true;
        }
        pending = true;
        take();
        continue;
      }
      if (!is_comment_start())
        break;
      if (!have_start)
      {
        start = peek(0);
        have_start = true;
      }
      pending = true;
      take();
      const int kind = take().cp;
      if (kind == '/')
      {
        while (peek(0).cp != kEof && peek(0).cp != '\n')
          take();
        continue;
      }

      bool comment_has_newline = false;
      for (;;)
      {
        if (peek(0).cp == kEof)
          throw std::runtime_error("unterminated block comment");
        if (peek(0).cp == '*' && peek(1).cp == '/')
        {
          take();
          take();
          break;
        }
        if (peek(0).cp == '\n')
        {
          const Unit newline = take();
          if (pending)
          {
            emit_whitespace(start);
            pending = false;
          }
          emit_comment_newline(newline);
          comment_has_newline = true;
        }
        else
          take();
      }
      if (comment_has_newline)
      {
        pending = false;
        have_start = false;
      }
    }
    if (pending)
      emit_whitespace(start);
  }

  std::string scan_identifier()
  {
    std::string spelling;
    int cp;
    std::size_t length;
    logical_peek(0, &cp, &length);
    if (!is_identifier_start(cp))
      throw std::logic_error("identifier scanner called at non-identifier");
    append_logical(&spelling);
    for (;;)
    {
      logical_peek(0, &cp, &length);
      if (!is_identifier_continue(cp))
        break;
      append_logical(&spelling);
    }
    return spelling;
  }

  std::string scan_pp_number()
  {
    std::string spelling;
    int previous = kEof;
    for (;;)
    {
      int cp;
      std::size_t length;
      logical_peek(0, &cp, &length);
      if (is_digit(cp) || is_identifier_continue(cp) || cp == '.')
      {
        append_logical(&spelling);
        previous = cp;
        continue;
      }
      if ((cp == '+' || cp == '-') && (previous == 'e' || previous == 'E'))
      {
        append_logical(&spelling);
        previous = cp;
        continue;
      }
      break;
    }
    return spelling;
  }

  StringStart detect_string_start()
  {
    StringStart result;
    int cp = peek(0).cp;
    if (cp == '\'')
    {
      result.present = true;
      result.quote = '\'';
      return result;
    }
    if (cp == 'u' && peek(1).cp == '8' && peek(2).cp == '\'')
    {
      // u8 is not a C++11 character-literal encoding prefix.
      return result;
    }
    if ((cp == 'u' || cp == 'U' || cp == 'L') && peek(1).cp == '\'')
    {
      result.present = true;
      result.prefix_length = 1;
      result.quote = '\'';
      return result;
    }

    if (cp == '"')
    {
      result.present = true;
      result.quote = '"';
      return result;
    }
    if (cp == 'R' && peek(1).cp == '"')
    {
      result.present = true;
      result.raw = true;
      result.prefix_length = 1;
      result.quote = '"';
      return result;
    }
    if (cp == 'u' && peek(1).cp == '8')
    {
      if (peek(2).cp == 'R' && peek(3).cp == '"')
      {
        result.present = true;
        result.raw = true;
        result.prefix_length = 3;
        return result;
      }
      if (peek(2).cp == '"')
      {
        result.present = true;
        result.prefix_length = 2;
        return result;
      }
      return result;
    }
    if ((cp == 'u' || cp == 'U' || cp == 'L') && peek(1).cp == 'R' &&
        peek(2).cp == '"')
    {
      result.present = true;
      result.raw = true;
      result.prefix_length = 2;
      return result;
    }
    if ((cp == 'u' || cp == 'U' || cp == 'L') && peek(1).cp == '"')
    {
      result.present = true;
      result.prefix_length = 1;
      return result;
    }
    return result;
  }

  void consume_escape(std::string * spelling)
  {
    take(); // backslash
    const int cp = peek(0).cp;
    if (cp == kEof || cp == '\n')
      throw std::runtime_error("invalid escape sequence");
    if (cp == 'x')
    {
      append_utf8(take().cp, spelling);
      if (!is_hex(peek(0).cp))
        throw std::runtime_error("hex escape has no digits");
      while (is_hex(peek(0).cp))
        append_utf8(take().cp, spelling);
      return;
    }
    if (is_octal(cp))
    {
      append_utf8(take().cp, spelling);
      for (int i = 1; i < 3 && is_octal(peek(0).cp); ++i)
        append_utf8(take().cp, spelling);
      return;
    }
    if (cp == '\'' || cp == '"' || cp == '?' || cp == '\\' ||
        cp == 'a' || cp == 'b' || cp == 'f' || cp == 'n' || cp == 'r' ||
        cp == 't' || cp == 'v')
    {
      append_utf8(take().cp, spelling);
      return;
    }
    throw std::runtime_error("invalid escape sequence");
  }

  std::string scan_quoted_literal(const StringStart & start)
  {
    std::string spelling;
    for (std::size_t i = 0; i < start.prefix_length; ++i)
      append_utf8(take().cp, &spelling);
    if (peek(0).cp != start.quote)
      throw std::logic_error("quoted literal prefix changed during scan");
    append_utf8(take().cp, &spelling);
    bool has_content = false;
    for (;;)
    {
      const int cp = peek(0).cp;
      if (cp == kEof || cp == '\n')
        throw std::runtime_error("unterminated string or character literal");
      if (cp == start.quote)
      {
        if (start.quote == '\'' && !has_content)
          throw std::runtime_error("empty character literal");
        append_utf8(take().cp, &spelling);
        return spelling;
      }
      if (cp == '\\')
      {
        Ucn ucn = inspect_ucn(0);
        if (ucn.present)
        {
          append_utf8(take_logical(), &spelling);
          has_content = true;
        }
        else
        {
          spelling.push_back('\\');
          consume_escape(&spelling);
          has_content = true;
        }
      }
      else
      {
        append_utf8(take().cp, &spelling);
        has_content = true;
      }
    }
  }

  static void update_location(int cp, std::size_t * line, std::size_t * column)
  {
    if (cp == '\n')
    {
      ++*line;
      *column = 1;
    }
    else
      ++*column;
  }

  std::string scan_raw_string(const StringStart & start)
  {
    std::string spelling;
    for (std::size_t i = 0; i < start.prefix_length; ++i)
      append_utf8(take().cp, &spelling);

    const Unit quote = take();
    if (quote.cp != '"')
      throw std::logic_error("raw string prefix changed during scan");
    append_utf8(quote.cp, &spelling);

    // The opening prefix is translated normally, so use the logical quote's
    // physical range rather than assuming that the prefix occupies adjacent
    // source bytes (a phase-2 splice may occur inside it).
    std::size_t pos = quote.end;
    std::vector<int> delimiter_cps;
    bool opened = false;
    std::size_t line = quote.line;
    std::size_t column = quote.column;
    update_location(quote.cp, &line, &column);
    while (pos < source_.size())
    {
      int cp;
      std::size_t next;
      decode_utf8(source_, pos, &cp, &next);
      if (cp == '(')
      {
        for (std::size_t i = 0; i < delimiter_cps.size(); ++i)
          update_location(delimiter_cps[i], &line, &column);
        update_location(cp, &line, &column);
        pos = next;
        opened = true;
        break;
      }
      if (cp == ' ' || cp == ')' || cp == '\\' || cp == '\t' ||
          cp == '\v' || cp == '\f' || cp == '\n')
        throw std::runtime_error("invalid raw string delimiter");
      delimiter_cps.push_back(cp);
      update_location(cp, &line, &column);
      if (delimiter_cps.size() > 16)
        throw std::runtime_error("raw string delimiter exceeds 16 characters");
      pos = next;
    }
    if (!opened)
      throw std::runtime_error("unterminated raw string literal");

    bool closed = false;
    while (pos < source_.size())
    {
      int cp;
      std::size_t next;
      decode_utf8(source_, pos, &cp, &next);
      if (cp == ')')
      {
        std::size_t candidate = next;
        bool match = true;
        for (std::size_t i = 0; i < delimiter_cps.size(); ++i)
        {
          int candidate_cp;
          std::size_t candidate_next;
          if (!decode_utf8(source_, candidate, &candidate_cp, &candidate_next) ||
              candidate_cp != delimiter_cps[i])
          {
            match = false;
            break;
          }
          candidate = candidate_next;
        }
        int quote_cp;
        std::size_t after_quote;
        if (match && decode_utf8(source_, candidate, &quote_cp, &after_quote) &&
            quote_cp == '"')
        {
          update_location(')', &line, &column);
          for (std::size_t i = 0; i < delimiter_cps.size(); ++i)
            update_location(delimiter_cps[i], &line, &column);
          update_location('"', &line, &column);
          pos = after_quote;
          closed = true;
          break;
        }
      }
      update_location(cp, &line, &column);
      pos = next;
    }
    if (!closed)
      throw std::runtime_error("unterminated raw string literal");

    const std::size_t end_byte = pos;
    // From the initial quote through the final quote, phase-1/2 spellings are
    // reverted; only the possibly spliced prefix is kept in translated form.
    spelling.append(source_, quote.end, end_byte - quote.end);
    cursor_.reset_after_raw_literal(end_byte, line, column);
    lookahead_.clear();
    return spelling;
  }

  bool scan_ud_suffix(std::string * suffix)
  {
    int cp;
    std::size_t length;
    logical_peek(0, &cp, &length);
    if (!is_identifier_start(cp))
      return false;
    *suffix = scan_identifier();
    return true;
  }

  std::string scan_header_name()
  {
    const int opener = peek(0).cp;
    const int closer = opener == '<' ? '>' : '"';
    std::string name;
    append_utf8(take().cp, &name);
    for (;;)
    {
      const int cp = peek(0).cp;
      if (cp == kEof || cp == '\n')
        throw std::runtime_error("unterminated header name");
      if (cp == closer)
      {
        append_utf8(take().cp, &name);
        return name;
      }
      append_logical(&name);
    }
  }

  bool scan_operator(std::string * op)
  {
    if (peek(0).cp == '<' && peek(1).cp == ':' && peek(2).cp == ':' &&
        peek(3).cp != ':' && peek(3).cp != '>')
    {
      take();
      op->assign("<");
      return true;
    }
    static const char * const operators[] = {
      "%:%:", "->*", "<<=", ">>=", "...", "##", "<:", ":>", "<%",
      "%>", "%:", ".*", "::", "->", "+=", "-=", "*=", "/=", "%=",
      "^=", "&=", "|=", "<<", ">>", "<=", ">=", "&&", "==", "!=",
      "||", "++", "--", "{", "}", "[", "]", "#", "(", ")", ";",
      ":", "?", ".", "+", "-", "*", "/", "%", "^", "&", "|", "~",
      "!", "=", "<", ">", ","
    };
    for (std::size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i)
    {
      const std::string candidate(operators[i]);
      bool match = true;
      for (std::size_t j = 0; j < candidate.size(); ++j)
        if (peek(j).cp != static_cast<unsigned char>(candidate[j]))
        {
          match = false;
          break;
        }
      if (!match)
        continue;
      for (std::size_t j = 0; j < candidate.size(); ++j)
        take();
      *op = candidate;
      return true;
    }
    return false;
  }

  void finish_token(int kind, bool identifier, const std::string & spelling)
  {
    if (line_start_)
    {
      if (kind == 0 && (spelling == "#" || spelling == "%:"))
        directive_ = 1;
      else
        directive_ = 0;
    }
    else if (directive_ == 1)
    {
      directive_ = identifier && spelling == "include" ? 2 : 0;
    }
    else if (directive_ == 2)
    {
      // The header-name alternative is handled before ordinary tokenization.
      directive_ = 0;
    }
    line_start_ = false;
  }
};

} // namespace

PPTokenizer::PPTokenizer(const std::string & source, IPPTokenStream & output)
  : source_(source), output_(output)
{}

void PPTokenizer::tokenize()
{
  Lexer(source_, output_).run();
}
