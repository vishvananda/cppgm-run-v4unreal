// (C) 2013 CPPGM Foundation. All rights reserved.
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <sys/stat.h>

#include "preprocess/expressions/ControlExpression.h"
#include "preprocess/tokens/IPPTokenStream.h"
#include "preprocess/tokens/PPTokenizer.h"
#include "preprocess/tokens/PostTokenPipeline.h"

using namespace std;

typedef pair<unsigned long long, unsigned long long> PreprocessorFileId;
bool GetPreprocessorFileId(const string& path, PreprocessorFileId& fileid)
{
  struct stat info;
  if (stat(path.c_str(), &info) != 0) return false;
  fileid = make_pair(static_cast<unsigned long long>(info.st_dev),
                     static_cast<unsigned long long>(info.st_ino));
  return true;
}

namespace {
enum TokenKind { TK_SPACE, TK_NEWLINE, TK_HEADER, TK_IDENTIFIER, TK_NUMBER,
  TK_CHARACTER, TK_UD_CHARACTER, TK_STRING, TK_UD_STRING, TK_PUNCT,
  TK_OTHER, TK_PLACEMARK };
struct PaintNode;
typedef shared_ptr<const PaintNode> Paint;
struct PaintNode
{
  string name;
  unsigned priority;
  Paint left, right;
  size_t size;
  PaintNode(const string & n, unsigned p, const Paint & l, const Paint & r)
    : name(n), priority(p), left(l), right(r), size(1 + (l ? l->size : 0) + (r ? r->size : 0)) {}
};

struct Token
{
  TokenKind kind;
  string text;
  shared_ptr<const string> file;
  size_t line, column, presumed_line;
  Paint unavailable;
  Paint inherited_paint;
  bool from_macro;
  bool paste_operator, stringize_operator;
  Token(TokenKind k = TK_OTHER, const string & s = string(), size_t l = 1,
        size_t c = 1) : kind(k), text(s), line(l), column(c), presumed_line(l),
                         from_macro(false), paste_operator(false), stringize_operator(false) {}
};

unsigned paint_priority(const string & name)
{
  unsigned h = 2166136261u;
  for (size_t i = 0; i < name.size(); ++i) { h ^= static_cast<unsigned char>(name[i]); h *= 16777619u; }
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
  return h;
}
Paint make_paint_node(const string & name, unsigned priority, const Paint & left, const Paint & right)
{ return Paint(new PaintNode(name, priority, left, right)); }
void paint_split(const Paint & root, const string & key, Paint * left, Paint * right)
{
  if (!root) { left->reset(); right->reset(); return; }
  if (key < root->name)
  {
    Paint a, b; paint_split(root->left, key, &a, &b);
    *left = a; *right = make_paint_node(root->name, root->priority, b, root->right);
  }
  else
  {
    Paint a, b; paint_split(root->right, key, &a, &b);
    *left = make_paint_node(root->name, root->priority, root->left, a); *right = b;
  }
}
bool has_paint(const Paint & root, const string & name)
{
  Paint node = root;
  while (node)
  {
    if (name == node->name) return true;
    node = name < node->name ? node->left : node->right;
  }
  return false;
}
Paint insert_paint_missing(const Paint & root, const string & name)
{
  if (!root) return make_paint_node(name, paint_priority(name), Paint(), Paint());
  if (name == root->name) return root;
  const unsigned priority = paint_priority(name);
  if (priority < root->priority)
  {
    Paint left, right; paint_split(root, name, &left, &right);
    return make_paint_node(name, priority, left, right);
  }
  if (name < root->name)
  {
    Paint left = insert_paint_missing(root->left, name);
    if (left == root->left) return root;
    return make_paint_node(root->name, root->priority, left, root->right);
  }
  Paint right = insert_paint_missing(root->right, name);
  if (right == root->right) return root;
  return make_paint_node(root->name, root->priority, root->left, right);
}
Paint insert_paint(const Paint & root, const string & name)
{
  if (has_paint(root, name)) return root;
  return insert_paint_missing(root, name);
}
void paint_collect(const Paint & root, vector<string> * out)
{
  if (!root) return;
  paint_collect(root->left, out); out->push_back(root->name); paint_collect(root->right, out);
}
Paint intersect_names(const Paint & a, const Paint & b)
{
  if (a == b) return a;
  vector<string> names; paint_collect(a && (!b || a->size <= b->size) ? a : b, &names);
  const Paint & other = a && (!b || a->size <= b->size) ? b : a;
  Paint result;
  for (size_t i = 0; i < names.size(); ++i)
    if (has_paint(other, names[i])) result = insert_paint(result, names[i]);
  return result;
}
Paint unite_paints(const Paint & a, const Paint & b)
{
  if (a == b) return a;
  vector<string> names; paint_collect(b, &names);
  Paint result = a;
  for (size_t i = 0; i < names.size(); ++i) result = insert_paint(result, names[i]);
  return result;
}
Paint difference_paints(const Paint & a, const Paint & b)
{
  if (!a || a == b) return Paint();
  vector<string> names; paint_collect(a, &names);
  Paint result;
  for (size_t i = 0; i < names.size(); ++i)
    if (!has_paint(b, names[i])) result = insert_paint(result, names[i]);
  return result;
}


bool is_space(const Token & t) { return t.kind == TK_SPACE || t.kind == TK_NEWLINE; }
bool is_identifier(const Token & t) { return t.kind == TK_IDENTIFIER; }
bool is_punct(const Token & t, const string & s)
{
  return t.kind == TK_PUNCT && (t.text == s ||
    (s == "#" && t.text == "%:") || (s == "##" && t.text == "%:%:"));
}
size_t skip_space(const vector<Token> & ts, size_t at)
{
  while (at < ts.size() && is_space(ts[at])) ++at;
  return at;
}
bool contains_name(const Paint & names, const string & name) { return has_paint(names, name); }
void add_name(Paint * names, const string & name) { *names = insert_paint(*names, name); }

class TokenCollector : public IPPTokenStream
{
public:
  vector<Token> tokens;
  size_t line, column;
  TokenCollector() : line(1), column(1) {}
  void set_source_line(size_t l) { line = l; column = 1; }
  void set_source_location(size_t l, size_t c) { line = l; column = c; }
  void emit_whitespace_sequence() { tokens.push_back(Token(TK_SPACE, "", line, column)); }
  void emit_new_line() { tokens.push_back(Token(TK_NEWLINE, "", line, column)); }
  void emit_comment_new_line() { tokens.push_back(Token(TK_SPACE, "", line, column)); }
  void emit_header_name(const string & s) { add(TK_HEADER, s); }
  void emit_identifier(const string & s) { add(TK_IDENTIFIER, s); }
  void emit_pp_number(const string & s) { add(TK_NUMBER, s); }
  void emit_character_literal(const string & s) { add(TK_CHARACTER, s); }
  void emit_user_defined_character_literal(const string & s) { add(TK_UD_CHARACTER, s); }
  void emit_string_literal(const string & s) { add(TK_STRING, s); }
  void emit_user_defined_string_literal(const string & s) { add(TK_UD_STRING, s); }
  void emit_preprocessing_op_or_punc(const string & s)
  {
    // Normalize the two directive/paste digraphs at the phase-4 boundary.
    add(TK_PUNCT, s == "%:" ? "#" : (s == "%:%:" ? "##" : s));
  }
  void emit_non_whitespace_char(const string & s) { add(TK_OTHER, s); }
  void emit_eof() {}
private:
  void add(TokenKind k, const string & s) { tokens.push_back(Token(k, s, line, column)); }
};

vector<Token> tokenize(const string & source)
{
  TokenCollector collector;
  PPTokenizer tokenizer(source, collector);
  tokenizer.tokenize();
  return collector.tokens;
}

string quote_string(const string & s)
{
  string result("\"");
  for (size_t i = 0; i < s.size(); ++i)
  {
    if (s[i] == '\\' || s[i] == '"') result.push_back('\\');
    if (s[i] == '\n') { result += "\\n"; continue; }
    if (s[i] == '\r') { result += "\\r"; continue; }
    result.push_back(s[i]);
  }
  result.push_back('"');
  return result;
}

bool decode_string_literal(const string & spelling, string * value)
{
  size_t quote = spelling.find('"');
  if (quote == string::npos || spelling.size() < quote + 2 ||
      spelling[spelling.size() - 1] != '"')
    return false;
  value->clear();
  for (size_t i = quote + 1; i + 1 < spelling.size(); ++i)
  {
    unsigned char c = static_cast<unsigned char>(spelling[i]);
    if (c != '\\') { value->push_back(static_cast<char>(c)); continue; }
    if (++i + 1 >= spelling.size()) return false;
    c = static_cast<unsigned char>(spelling[i]);
    switch (c)
    {
      case '\\': value->push_back('\\'); break;
      case '\'': value->push_back('\''); break;
      case '"': value->push_back('"'); break;
      case '?': value->push_back('?'); break;
      case 'a': value->push_back('\a'); break;
      case 'b': value->push_back('\b'); break;
      case 'f': value->push_back('\f'); break;
      case 'n': value->push_back('\n'); break;
      case 'r': value->push_back('\r'); break;
      case 't': value->push_back('\t'); break;
      case 'v': value->push_back('\v'); break;
      case 'x':
      {
        unsigned number = 0; size_t digits = 0;
        while (i + 1 < spelling.size() - 1 && isxdigit(static_cast<unsigned char>(spelling[i + 1])))
        {
          const char h = spelling[++i];
          number = number * 16 + (h <= '9' ? h - '0' : (tolower(static_cast<unsigned char>(h)) - 'a' + 10));
          ++digits;
        }
        if (!digits || number > 255) return false;
        value->push_back(static_cast<char>(number));
        break;
      }
      default:
        if (c >= '0' && c <= '7')
        {
          unsigned number = c - '0';
          for (int n = 0; n < 2 && i + 1 < spelling.size() - 1 &&
               spelling[i + 1] >= '0' && spelling[i + 1] <= '7'; ++n)
            number = number * 8 + (spelling[++i] - '0');
          if (number > 255) return false;
          value->push_back(static_cast<char>(number));
        }
        else return false;
    }
  }
  return true;
}

struct Macro
{
  enum Builtin { NONE, LINE, FILE, DATE, TIME, COUNTER, HAS_ATTRIBUTE } builtin;
  bool function_like, variadic;
  vector<string> params;
  vector<Token> replacement;
  Macro() : builtin(NONE), function_like(false), variadic(false) {}
};

struct FileIdHash
{
  size_t operator()(const PreprocessorFileId & p) const
  { return static_cast<size_t>(p.first ^ (p.second + 0x9e3779b97f4a7c15ULL + (p.first<<6) + (p.first>>2))); }
};

class Preprocessor
{
public:
  explicit Preprocessor(const string & date, const string & time)
    : date_(date), time_(time), counter_(0), post_(0) {}

  void begin_translation_unit(IPPTokenStream & output)
  {
    macros_.clear();
    once_.clear();
    counter_ = 0;
    post_ = &output;
    define_builtin("__CPPGM__", "201303L");
    define_builtin("__cplusplus", "201103L");
    define_builtin("__STDC_HOSTED__", "1");
    define_builtin("__CPPGM_AUTHOR__", "\"Unreal Labs\"");
    define_builtin("__FILE__", "", Macro::FILE);
    define_builtin("__LINE__", "", Macro::LINE);
    define_builtin("__DATE__", date_, Macro::DATE);
    define_builtin("__TIME__", time_, Macro::TIME);
    define_builtin("__COUNTER__", "", Macro::COUNTER);
    Macro attr; attr.function_like = true; attr.variadic = true;
    attr.builtin = Macro::HAS_ATTRIBUTE;
    macros_["__has_cpp_attribute"] = attr;
  }

  void run_primary(const string & path)
  {
    process_file(path, 0);
  }

private:
  string date_, time_;
  unordered_map<string, Macro> macros_;
  unordered_set<PreprocessorFileId, FileIdHash> once_;
  unsigned long long counter_;
  IPPTokenStream * post_;

  void define_builtin(const string & name, const string & replacement,
                      Macro::Builtin builtin = Macro::NONE)
  {
    Macro m; m.builtin = builtin;
    if (builtin == Macro::NONE)
    {
      try { m.replacement = tokenize(replacement); }
      catch (...) { m.replacement.clear(); }
    }
    macros_[name] = m;
  }

  void locate(Token * t, const shared_ptr<const string> & file, long long line_delta)
  {
    t->file = file;
    long long line = static_cast<long long>(t->line) + line_delta;
    t->presumed_line = line > 0 ? static_cast<size_t>(line) : 1;
  }

  static bool same_definition(const Macro & a, const Macro & b)
  {
    if (a.builtin != Macro::NONE || b.builtin != Macro::NONE)
      return a.builtin == b.builtin;
    if (a.function_like != b.function_like || a.variadic != b.variadic ||
        a.params.size() != b.params.size() || a.replacement.size() != b.replacement.size())
      return false;
    unordered_map<string, size_t> amap, bmap;
    for (size_t i = 0; i < a.params.size(); ++i)
    {
      if (a.params[i] != b.params[i]) return false;
      amap[a.params[i]] = i; bmap[b.params[i]] = i;
    }
    for (size_t i = 0; i < a.replacement.size(); ++i)
    {
      const Token & x = a.replacement[i], &y = b.replacement[i];
      if (x.kind != y.kind) return false;
      string xs = x.text, ys = y.text;
      if (x.kind == TK_IDENTIFIER && amap.count(xs)) xs = "$" + to_string(amap[xs]);
      if (y.kind == TK_IDENTIFIER && bmap.count(ys)) ys = "$" + to_string(bmap[ys]);
      if (xs != ys) return false;
    }
    return true;
  }

  static vector<Token> normalize_replacement(const vector<Token> & src)
  {
    vector<Token> result;
    for (size_t i = 0; i < src.size(); ++i)
    {
      if (is_space(src[i]))
      {
        if (!result.empty() && result.back().kind != TK_SPACE)
          result.push_back(Token(TK_SPACE));
      }
      else result.push_back(src[i]);
    }
    while (!result.empty() && result.back().kind == TK_SPACE) result.pop_back();
    return result;
  }

  static string canonical_parameter(const Token & t,
                                    const unordered_map<string, size_t> & params)
  {
    if (t.kind == TK_IDENTIFIER && params.count(t.text))
      return "$" + to_string(params.find(t.text)->second);
    return t.text;
  }

  void define_macro(const vector<Token> & line, size_t at)
  {
    if (at >= line.size() || !is_identifier(line[at])) throw runtime_error("invalid #define");
    const string name = line[at].text;
    if (name == "__VA_ARGS__") throw runtime_error("invalid macro name");
    ++at;
    Macro m;
    if (at < line.size() && line[at].kind == TK_PUNCT && line[at].text == "(")
    {
      m.function_like = true;
      ++at;
      at = skip_space(line, at);
      if (at < line.size() && line[at].kind == TK_PUNCT && line[at].text == ")")
        ++at;
      else
      {
        bool need_param = true;
        for (;;)
        {
          at = skip_space(line, at);
          if (at >= line.size()) throw runtime_error("unterminated macro parameter list");
          if (is_punct(line[at], "..."))
          {
            if (!need_param) throw runtime_error("invalid variadic macro parameter list");
            m.variadic = true; ++at; at = skip_space(line, at);
            if (at >= line.size() || line[at].kind != TK_PUNCT || line[at].text != ")")
              throw runtime_error("variadic parameter must be last");
            ++at; break;
          }
          if (!is_identifier(line[at]) || line[at].text == "__VA_ARGS__")
            throw runtime_error("invalid macro parameter");
          const string param = line[at++].text;
          if (find(m.params.begin(), m.params.end(), param) != m.params.end())
            throw runtime_error("duplicate macro parameter");
          m.params.push_back(param); need_param = false;
          at = skip_space(line, at);
          if (at >= line.size()) throw runtime_error("unterminated macro parameter list");
          if (line[at].kind == TK_PUNCT && line[at].text == ")") { ++at; break; }
          if (line[at].kind == TK_PUNCT && line[at].text == ",") { ++at; need_param = true; continue; }
          throw runtime_error("invalid macro parameter list");
        }
      }
    }
    if (m.function_like && m.variadic) { /* __VA_ARGS__ is an implicit tail parameter */ }
    vector<Token> raw(line.begin() + at, line.end());
    m.replacement = normalize_replacement(raw);
    for (size_t i = 0; i < m.replacement.size(); ++i)
    {
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].text == "##") m.replacement[i].paste_operator = true;
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].text == "#") m.replacement[i].stringize_operator = true;
    }
    unordered_map<string, size_t> param_map;
    for (size_t i = 0; i < m.params.size(); ++i) param_map[m.params[i]] = i;
    for (size_t i = 0; i < m.replacement.size(); ++i)
    {
      if (m.replacement[i].kind == TK_IDENTIFIER && m.replacement[i].text == "__VA_ARGS__" && !m.variadic)
        throw runtime_error("__VA_ARGS__ used outside variadic macro");
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].text == "#")
      {
        size_t before = i; while (before && is_space(m.replacement[before - 1])) --before;
        size_t after = i + 1; while (after < m.replacement.size() && is_space(m.replacement[after])) ++after;
        const bool paste_operand = (before && m.replacement[before - 1].kind == TK_PUNCT && m.replacement[before - 1].text == "##") ||
          (after < m.replacement.size() && m.replacement[after].kind == TK_PUNCT && m.replacement[after].text == "##");
        if (!paste_operand)
        {
          if (!m.function_like || after >= m.replacement.size() ||
              !(is_identifier(m.replacement[after]) && (param_map.count(m.replacement[after].text) ||
                (m.variadic && m.replacement[after].text == "__VA_ARGS__"))))
            throw runtime_error("invalid stringizing operator");
        }
      }
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].text == "##")
      {
        size_t left = i;
        while (left && is_space(m.replacement[left - 1])) --left;
        size_t right = i + 1;
        while (right < m.replacement.size() && is_space(m.replacement[right])) ++right;
        if (left == 0 || right == m.replacement.size())
          throw runtime_error("invalid token paste operator");
      }
    }
    const unordered_map<string, Macro>::iterator found = macros_.find(name);
    if (found != macros_.end())
    {
      if (!same_definition(found->second, m)) throw runtime_error("incompatible macro redefinition");
      return;
    }
    macros_[name] = m;
  }

  bool parse_arguments(const deque<Token> & work, size_t open_index,
                       vector<vector<Token> > * args, size_t * consumed)
  {
    args->clear();
    vector<Token> current;
    int depth = 0;
    bool saw_any = false;
    for (size_t i = open_index + 1; i < work.size(); ++i)
    {
      const Token & t = work[i];
      if (t.kind == TK_PUNCT && t.text == "(") { ++depth; current.push_back(t); saw_any = true; }
      else if (t.kind == TK_PUNCT && t.text == ")")
      {
        if (depth == 0)
        {
          if (saw_any || !current.empty()) args->push_back(current);
          *consumed = i + 1;
          return true;
        }
        --depth; current.push_back(t); saw_any = true;
      }
      else if (t.kind == TK_PUNCT && t.text == "," && depth == 0)
      {
        args->push_back(current); current.clear(); saw_any = true;
      }
      else { current.push_back(t); if (!is_space(t)) saw_any = true; }
    }
    return false;
  }

  Token pasted_token(const Token & left, const Token & right, const Token & head,
                     const string & macro_name)
  {
    const string combined = left.text + right.text;
    vector<Token> lexed = tokenize(combined);
    vector<Token> significant;
    for (size_t i = 0; i < lexed.size(); ++i)
      if (!is_space(lexed[i])) significant.push_back(lexed[i]);
    if (significant.size() != 1 || significant[0].kind == TK_OTHER ||
        significant[0].kind == TK_HEADER)
      throw runtime_error(string("invalid token paste in ") + macro_name + " [" + combined + "]");
    Token result = significant[0];
    result.file = head.file; result.line = head.line; result.presumed_line = head.presumed_line;
    result.unavailable = unite_paints(left.unavailable, right.unavailable);
    result.inherited_paint = head.unavailable;
    result.from_macro = true;
    (void)macro_name;
    return result;
  }

  vector<Token> apply_token_paste(const vector<Token> & substituted,
                                  const Macro & macro, const Token & head,
                                  const string & name)
  {
    vector<Token> pasted;
    for (size_t q = 0; q < substituted.size(); ++q)
    {
      if (!substituted[q].paste_operator)
      { pasted.push_back(substituted[q]); continue; }
      while (!pasted.empty() && is_space(pasted.back())) pasted.pop_back();
      size_t r = q + 1;
      while (r < substituted.size() && is_space(substituted[r])) ++r;
      if (pasted.empty() || r >= substituted.size()) throw runtime_error("invalid token paste");
      Token left = pasted.back(); pasted.pop_back();
      Token right = substituted[r]; q = r;
      if (macro.variadic && left.kind == TK_PUNCT && left.text == ",")
      {
        // GNU's comma elision extension: an empty variadic tail removes the
        // comma, while a nonempty tail retains the comma and all tail tokens.
        if (right.kind == TK_PLACEMARK) continue;
        pasted.push_back(left);
        q = r - 1;
        continue;
      }
      if (left.kind == TK_PLACEMARK && right.kind == TK_PLACEMARK)
        pasted.push_back(Token(TK_PLACEMARK, ""));
      else if (left.kind == TK_PLACEMARK) pasted.push_back(right);
      else if (right.kind == TK_PLACEMARK) pasted.push_back(left);
      else pasted.push_back(pasted_token(left, right, head, name));
    }
    return pasted;
  }

  enum BuiltinResult { BUILTIN_REPLACED, BUILTIN_NOT_INVOKED, BUILTIN_DEFERRED };

  BuiltinResult expand_builtin(Token & head, const string & name,
                               const Macro & macro, deque<Token> & work,
                               bool final, vector<Token> * deferred)
  {
    bool known_attribute = false;
    if (macro.function_like)
    {
      size_t open = 0;
      while (open < work.size() && is_space(work[open])) ++open;
      if (open >= work.size() && !final)
      {
        if (deferred) deferred->push_back(std::move(head));
        while (!work.empty()) { if (deferred) deferred->push_back(std::move(work.front())); work.pop_front(); }
        return BUILTIN_DEFERRED;
      }
      if (open >= work.size() || work[open].kind != TK_PUNCT || work[open].text != "(")
        return BUILTIN_NOT_INVOKED;
      vector<vector<Token> > builtin_args; size_t consumed = 0;
      if (!parse_arguments(work, open, &builtin_args, &consumed))
      {
        if (final) throw runtime_error("unterminated builtin macro invocation");
        work.push_front(std::move(head));
        while (!work.empty()) { if (deferred) deferred->push_back(std::move(work.front())); work.pop_front(); }
        return BUILTIN_DEFERRED;
      }
      for (size_t i = 0; i < consumed; ++i) work.pop_front();
      if (macro.builtin == Macro::HAS_ATTRIBUTE && !builtin_args.empty())
      {
        size_t ai = skip_space(builtin_args[0], 0);
        if (ai < builtin_args[0].size() && is_identifier(builtin_args[0][ai]) &&
            (builtin_args[0][ai].text == "no_unique_address" ||
             builtin_args[0][ai].text == "__no_unique_address__"))
          known_attribute = true;
      }
    }
    Token replacement;
    if (macro.builtin == Macro::LINE)
      replacement = Token(TK_NUMBER, to_string(head.presumed_line), head.line, head.column);
    else if (macro.builtin == Macro::FILE)
      replacement = Token(TK_STRING, quote_string(head.file ? *head.file : string()), head.line, head.column);
    else if (macro.builtin == Macro::DATE)
      replacement = Token(TK_STRING, macro.replacement.empty() ? date_ : macro.replacement[0].text, head.line, head.column);
    else if (macro.builtin == Macro::TIME)
      replacement = Token(TK_STRING, macro.replacement.empty() ? time_ : macro.replacement[0].text, head.line, head.column);
    else if (macro.builtin == Macro::COUNTER)
      replacement = Token(TK_NUMBER, to_string(counter_++), head.line, head.column);
    else
      replacement = Token(TK_NUMBER, known_attribute ? "201803" : "0", head.line, head.column);
    replacement.file = head.file; replacement.presumed_line = head.presumed_line;
    replacement.unavailable = head.unavailable; add_name(&replacement.unavailable, name);
    replacement.inherited_paint = head.unavailable; replacement.from_macro = true;
    work.push_front(std::move(replacement));
    return BUILTIN_REPLACED;
  }

  vector<Token> expand(vector<Token> input, unsigned depth = 0,
                       bool * contains_pragma_operator = 0, bool final = true,
                       vector<Token> * deferred = 0)
  {
    if (depth > 256) throw runtime_error("macro argument expansion nesting limit");
    bool possible_macro = false, found_pragma_operator = false;
    for (size_t i = 0; i < input.size(); ++i)
    {
      if (is_identifier(input[i]) && input[i].text == "_Pragma") found_pragma_operator = true;
      if (is_identifier(input[i]) && !contains_name(input[i].unavailable, input[i].text) &&
          macros_.find(input[i].text) != macros_.end()) possible_macro = true;
    }
    if (!possible_macro && (final || !found_pragma_operator))
    {
      if (contains_pragma_operator) *contains_pragma_operator = found_pragma_operator;
      return input;
    }
    deque<Token> work;
    for (size_t i = 0; i < input.size(); ++i) work.push_back(std::move(input[i]));
    vector<Token>().swap(input);
    vector<Token> output;
    size_t expansions = 0;
    while (!work.empty())
    {
      if (++expansions > 10000000) throw runtime_error("macro expansion work limit");
      Token head = std::move(work.front()); work.pop_front();
      if (!is_identifier(head) || contains_name(head.unavailable, head.text))
      { output.push_back(std::move(head)); continue; }
      if (!final && head.text == "_Pragma")
      {
        if (deferred) deferred->push_back(std::move(head));
        while (!work.empty()) { if (deferred) deferred->push_back(std::move(work.front())); work.pop_front(); }
        break;
      }
      unordered_map<string, Macro>::const_iterator found = macros_.find(head.text);
      if (found == macros_.end()) { output.push_back(std::move(head)); continue; }
      const string name = head.text;
      const Macro & macro = found->second;

      if (macro.builtin != Macro::NONE)
      {
        const BuiltinResult result = expand_builtin(head, name, macro, work, final, deferred);
        if (result == BUILTIN_NOT_INVOKED) output.push_back(std::move(head));
        if (result == BUILTIN_DEFERRED) break;
        continue;
      }

      vector<Token> body;
      size_t consumed = 0;
      Paint macro_paint;
      vector<vector<Token> > args;
      if (macro.function_like)
      {
        size_t open = 0;
        while (open < work.size() && is_space(work[open])) ++open;
        if (open >= work.size() && !final)
        {
          if (deferred) deferred->push_back(std::move(head));
          while (!work.empty()) { if (deferred) deferred->push_back(std::move(work.front())); work.pop_front(); }
          break;
        }
        if (open >= work.size() || work[open].kind != TK_PUNCT || work[open].text != "(")
        { output.push_back(std::move(head)); continue; }
        if (!parse_arguments(work, open, &args, &consumed))
        {
          if (final) throw runtime_error("unterminated function macro invocation");
          work.push_front(std::move(head));
          while (!work.empty()) { if (deferred) deferred->push_back(std::move(work.front())); work.pop_front(); }
          break;
        }
        const Paint & closing_paint = work[consumed - 1].unavailable;
        macro_paint = intersect_names(head.unavailable, closing_paint);
        add_name(&macro_paint, name);
        const size_t fixed = macro.params.size();
        if (fixed == 0 && !macro.variadic && args.size() == 1 &&
            skip_space(args[0], 0) == args[0].size()) args.clear();
        else if (args.empty() && fixed != 0) args.push_back(vector<Token>());
        if ((!macro.variadic && args.size() != fixed) ||
            (macro.variadic && args.size() < fixed))
          throw runtime_error("wrong macro argument count");
        for (size_t i = 0; i < consumed; ++i) work.pop_front();

        vector<vector<Token> > expanded_args(fixed + (macro.variadic ? 1 : 0));
        vector<bool> expanded_ready(expanded_args.size(), false);
        vector<Token> substituted;
        unordered_map<string, size_t> param_index;
        for (size_t i = 0; i < fixed; ++i) param_index[macro.params[i]] = i;
        if (macro.variadic) param_index["__VA_ARGS__"] = fixed;
        for (size_t ri = 0; ri < macro.replacement.size(); ++ri)
        {
          const Token & rt = macro.replacement[ri];
          size_t hash_after = ri + 1;
          while (hash_after < macro.replacement.size() && is_space(macro.replacement[hash_after])) ++hash_after;
          size_t hash_before = ri; while (hash_before && is_space(macro.replacement[hash_before - 1])) --hash_before;
          const bool stringize_candidate = rt.stringize_operator &&
            hash_after < macro.replacement.size() && is_identifier(macro.replacement[hash_after]) &&
            param_index.count(macro.replacement[hash_after].text);
          const bool hash_is_paste = rt.kind == TK_PUNCT && rt.text == "#" && !stringize_candidate &&
            ((hash_after < macro.replacement.size() && macro.replacement[hash_after].kind == TK_PUNCT && macro.replacement[hash_after].text == "##") ||
             (hash_before && macro.replacement[hash_before - 1].kind == TK_PUNCT && macro.replacement[hash_before - 1].text == "##"));
          if (stringize_candidate && !hash_is_paste)
          {
            size_t pi = skip_space(macro.replacement, ri + 1);
            const string pname = macro.replacement[pi].text;
            size_t ix = param_index.at(pname);
            vector<Token> raw;
            if (ix < fixed) raw = args[ix];
            else
            {
              for (size_t ai = fixed; ai < args.size(); ++ai)
              {
                if (ai != fixed) raw.push_back(Token(TK_PUNCT, ","));
                raw.insert(raw.end(), args[ai].begin(), args[ai].end());
              }
            }
            string content; bool pending_space = false, have = false;
            for (size_t q = 0; q < raw.size(); ++q)
            {
              if (is_space(raw[q])) { if (have) pending_space = true; continue; }
              if (pending_space) content.push_back(' ');
              pending_space = false; have = true;
              for (size_t z = 0; z < raw[q].text.size(); ++z)
              {
                const bool literal_token = raw[q].kind == TK_STRING || raw[q].kind == TK_UD_STRING ||
                  raw[q].kind == TK_CHARACTER || raw[q].kind == TK_UD_CHARACTER;
                if (literal_token && (raw[q].text[z] == '\\' || raw[q].text[z] == '"'))
                  content.push_back('\\');
                content.push_back(raw[q].text[z]);
              }
            }
            Token stringized(TK_STRING, string("\"") + content + "\"", head.line, head.column);
            stringized.file = head.file; stringized.presumed_line = head.presumed_line;
            stringized.unavailable = macro_paint;
            stringized.inherited_paint = head.unavailable; stringized.from_macro = true;
            substituted.push_back(stringized); ri = pi;
            continue;
          }
          if (is_identifier(rt) && param_index.count(rt.text))
          {
            const size_t ix = param_index[rt.text];
            size_t prev = ri; while (prev && is_space(macro.replacement[prev - 1])) --prev;
            size_t next = ri + 1; while (next < macro.replacement.size() && is_space(macro.replacement[next])) ++next;
            const bool pasted = (prev && macro.replacement[prev - 1].kind == TK_PUNCT && macro.replacement[prev - 1].text == "##") ||
              (next < macro.replacement.size() && macro.replacement[next].kind == TK_PUNCT && macro.replacement[next].text == "##");
            vector<Token> values;
            if (ix < fixed) values = args[ix];
            else
            {
              for (size_t ai = fixed; ai < args.size(); ++ai)
              {
                if (ai != fixed) values.push_back(Token(TK_PUNCT, ","));
                values.insert(values.end(), args[ai].begin(), args[ai].end());
              }
            }
            if (values.empty() && pasted) values.push_back(Token(TK_PLACEMARK, ""));
            else if (!pasted)
            {
              if (!expanded_ready[ix]) { expanded_args[ix] = expand(std::move(values), depth + 1); expanded_ready[ix] = true; }
              values = expanded_args[ix];
            }
            for (size_t q = 0; q < values.size(); ++q)
            {
              Token value = values[q];
              if (is_identifier(value) && macros_.find(value.text) != macros_.end())
              {
                if (value.from_macro)
                {
                  value.unavailable = difference_paints(value.unavailable, value.inherited_paint);
                  Paint current; add_name(&current, name);
                  value.unavailable = unite_paints(value.unavailable, current);
                }
                else
                {
                  value.unavailable = unite_paints(value.unavailable, head.unavailable);
                  value.unavailable = unite_paints(value.unavailable, macro_paint);
                }
                value.inherited_paint = head.unavailable;
                value.from_macro = true;
              }
              substituted.push_back(value);
            }
            continue;
          }
          Token copy = rt;
          copy.file = head.file; copy.line = head.line; copy.column = head.column;
          copy.presumed_line = head.presumed_line;
          copy.unavailable = macro_paint;
          copy.inherited_paint = head.unavailable; copy.from_macro = true;
          substituted.push_back(copy);
        }
        vector<Token> pasted = apply_token_paste(substituted, macro, head, name);
        for (size_t q = 0; q < pasted.size(); ++q)
          if (pasted[q].kind != TK_PLACEMARK) body.push_back(pasted[q]);
      }
      else
      {
        body = macro.replacement;
        for (size_t q = 0; q < body.size(); ++q)
        {
          body[q].file = head.file; body[q].line = head.line; body[q].column = head.column;
          body[q].presumed_line = head.presumed_line;
          body[q].unavailable = head.unavailable; add_name(&body[q].unavailable, name);
          body[q].inherited_paint = head.unavailable; body[q].from_macro = true;
        }
        bool has_paste = false;
        for (size_t q = 0; q < body.size(); ++q)
          if (body[q].paste_operator) has_paste = true;
        if (has_paste)
        {
          vector<Token> pasted = apply_token_paste(body, macro, head, name);
          body.clear();
          for (size_t q = 0; q < pasted.size(); ++q)
            if (pasted[q].kind != TK_PLACEMARK) body.push_back(pasted[q]);
        }
      }
      for (size_t q = body.size(); q > 0; --q) work.push_front(std::move(body[q - 1]));
    }
    if (contains_pragma_operator)
    {
      if (!found_pragma_operator)
        for (size_t i = 0; i < output.size(); ++i)
          if (is_identifier(output[i]) && output[i].text == "_Pragma") { found_pragma_operator = true; break; }
      *contains_pragma_operator = found_pragma_operator;
    }
    return output;
  }

  bool evaluate_if(vector<Token> expression)
  {
    // `defined` binds before macro expansion and its operand is protected.
    vector<Token> protected_tokens;
    for (size_t i = 0; i < expression.size(); ++i)
    {
      if (is_identifier(expression[i]) && expression[i].text == "defined")
      {
        size_t j = skip_space(expression, i + 1); bool paren = false;
        if (j < expression.size() && expression[j].kind == TK_PUNCT && expression[j].text == "(")
        { paren = true; j = skip_space(expression, j + 1); }
        if (j >= expression.size() || !(is_identifier(expression[j]) ||
            (expression[j].kind == TK_PUNCT && is_alternative_word(expression[j].text))))
          throw runtime_error("invalid defined operator");
        const string operand = expression[j].text;
        const bool exists = macros_.find(operand) != macros_.end();
        ++j; j = skip_space(expression, j);
        if (paren)
        {
          if (j >= expression.size() || expression[j].kind != TK_PUNCT || expression[j].text != ")")
            throw runtime_error("invalid defined operator");
          ++j;
        }
        Token value(TK_NUMBER, exists ? "1" : "0", expression[i].line, expression[i].column);
        value.file = expression[i].file; value.presumed_line = expression[i].presumed_line;
        protected_tokens.push_back(value); i = j - 1;
      }
      else protected_tokens.push_back(expression[i]);
    }
    vector<Token> expanded = expand(std::move(protected_tokens));
    vector<ControlExpressionToken> operands;
    for (size_t i = 0; i < expanded.size(); ++i)
    {
      const Token & t = expanded[i];
      if (is_space(t)) continue;
      if (t.kind == TK_IDENTIFIER)
      {
        if (t.text == "true" || t.text == "false")
          operands.push_back(ControlExpressionToken(ControlExpressionToken::IDENTIFIER, t.text));
        else operands.push_back(ControlExpressionToken(ControlExpressionToken::PP_NUMBER, "0"));
      }
      else if (t.kind == TK_NUMBER)
        operands.push_back(ControlExpressionToken(ControlExpressionToken::PP_NUMBER, t.text));
      else if (t.kind == TK_CHARACTER)
        operands.push_back(ControlExpressionToken(ControlExpressionToken::CHARACTER_LITERAL, t.text));
      else if (t.kind == TK_PUNCT)
        operands.push_back(ControlExpressionToken(ControlExpressionToken::OPERATOR, t.text));
      else operands.push_back(ControlExpressionToken(ControlExpressionToken::INVALID, t.text));
    }
    bool result = false;
    if (!evaluate_control_expression_tokens(operands, &result))
      throw runtime_error("invalid controlling expression");
    return result;
  }

  static bool is_alternative_word(const string & s)
  {
    static const char * words[] = {"and", "and_eq", "bitand", "bitor", "compl", "not", "not_eq", "or", "or_eq", "xor", "xor_eq"};
    for (size_t i = 0; i < sizeof(words)/sizeof(words[0]); ++i) if (s == words[i]) return true;
    return false;
  }

  void mark_pragma_once(const string & filename)
  {
    PreprocessorFileId id;
    if (GetPreprocessorFileId(filename, id)) once_.insert(id);
  }

  void execute_pragma_operator(vector<Token> * tokens, const string & fallback_file)
  {
    bool has_pragma_operator = false;
    for (size_t i = 0; i < tokens->size(); ++i)
      if (is_identifier((*tokens)[i]) && (*tokens)[i].text == "_Pragma") { has_pragma_operator = true; break; }
    if (!has_pragma_operator) return;
    vector<Token> out;
    for (size_t i = 0; i < tokens->size(); ++i)
    {
      const Token & t = (*tokens)[i];
      if (!is_identifier(t) || t.text != "_Pragma") { out.push_back(t); continue; }
      size_t j = skip_space(*tokens, i + 1);
      if (j >= tokens->size() || (*tokens)[j].kind != TK_PUNCT || (*tokens)[j].text != "(")
        throw runtime_error("invalid _Pragma operator");
      j = skip_space(*tokens, j + 1);
      if (j >= tokens->size() || ((*tokens)[j].kind != TK_STRING && (*tokens)[j].kind != TK_UD_STRING))
        throw runtime_error("invalid _Pragma operand");
      string pragma;
      if (!decode_string_literal((*tokens)[j].text, &pragma)) throw runtime_error("invalid _Pragma string");
      size_t end = skip_space(*tokens, j + 1);
      if (end >= tokens->size() || (*tokens)[end].kind != TK_PUNCT || (*tokens)[end].text != ")")
        throw runtime_error("invalid _Pragma invocation");
      if (pragma == "once") mark_pragma_once(t.file ? *t.file : fallback_file);
      i = end;
    }
    tokens->swap(out);
  }

  void emit_text(vector<Token> & tokens, const string & file, bool final = true,
                 vector<Token> * deferred = 0)
  {
    if (tokens.empty()) return;
    bool has_pragma_operator = false;
    vector<Token> expanded = expand(std::move(tokens), 0, &has_pragma_operator,
                                    final, deferred);
    if (final && has_pragma_operator) execute_pragma_operator(&expanded, file);
    for (size_t i = 0; i < expanded.size(); ++i)
    {
      const Token & t = expanded[i];
      post_->set_source_file(t.file ? *t.file : string());
      post_->set_source_location(t.presumed_line, t.column);
      switch (t.kind)
      {
        case TK_SPACE: case TK_NEWLINE: post_->emit_whitespace_sequence(); break;
        case TK_HEADER: post_->emit_header_name(t.text); break;
        case TK_IDENTIFIER:
          if (t.text == "__VA_ARGS__") throw runtime_error("__VA_ARGS__ outside variadic macro");
          post_->emit_identifier(t.text); break;
        case TK_NUMBER: post_->emit_pp_number(t.text); break;
        case TK_CHARACTER: post_->emit_character_literal(t.text); break;
        case TK_UD_CHARACTER: post_->emit_user_defined_character_literal(t.text); break;
        case TK_STRING: post_->emit_string_literal(t.text); break;
        case TK_UD_STRING: post_->emit_user_defined_string_literal(t.text); break;
        case TK_PUNCT: post_->emit_preprocessing_op_or_punc(t.text); break;
        case TK_OTHER: post_->emit_non_whitespace_char(t.text); break;
        case TK_PLACEMARK: break;
      }
    }
  }

  string include_path(const string & current, const vector<Token> & args)
  {
    vector<Token> expanded = expand(std::move(args));
    size_t start = skip_space(expanded, 0), end = expanded.size();
    while (end && is_space(expanded[end - 1])) --end;
    if (start >= end) throw runtime_error("empty include operand");
    string next;
    if (end == start + 1 && expanded[start].kind == TK_HEADER)
    {
      const string & h = expanded[start].text;
      if (h.size() < 2 || (h[0] != '<' && h[0] != '"')) throw runtime_error("invalid header name");
      next = h.substr(1, h.size() - 2);
    }
    else if (end == start + 1 && expanded[start].kind == TK_STRING)
    {
      if (!decode_string_literal(expanded[start].text, &next)) throw runtime_error("invalid include string");
    }
    else throw runtime_error("invalid include operand");
    string pathrel;
    const size_t slash = current.find_last_of('/');
    if (slash != string::npos) pathrel = current.substr(0, slash + 1) + next;
    if (!pathrel.empty())
    {
      ifstream candidate(pathrel.c_str(), ios::binary);
      if (candidate.good()) return pathrel;
    }
    ifstream direct(next.c_str(), ios::binary);
    if (direct.good()) return next;
    throw runtime_error("include file not found");
  }

  struct Conditional
  {
    bool parent_active, active, taken, saw_else;
    Conditional(bool p, bool a) : parent_active(p), active(a), taken(a), saw_else(false) {}
  };

  struct FileStream;
  void process_line(FileStream &);

  struct FileStream : IPPTokenStream
  {
    Preprocessor & owner;
    unsigned include_depth;
    string presumed_file;
    shared_ptr<const string> presumed_file_ref;
    long long line_delta;
    size_t line, column, physical_end_line;
    vector<Token> current, pending_text;
    vector<Conditional> conditions;

    FileStream(Preprocessor & p, const string & path, unsigned depth)
      : owner(p), include_depth(depth), presumed_file(path),
        presumed_file_ref(new string(path)), line_delta(0),
        line(1), column(1), physical_end_line(1) {}

    void set_source_line(size_t l) { line = l; column = 1; }
    void set_source_location(size_t l, size_t c) { line = l; column = c; }
    void emit_whitespace_sequence() { add(TK_SPACE, ""); }
    void emit_new_line()
    {
      physical_end_line = line;
      owner.process_line(*this);
      current.clear();
    }
    void emit_comment_new_line() { add(TK_SPACE, ""); }
    void emit_header_name(const string & s) { add(TK_HEADER, s); }
    void emit_identifier(const string & s) { add(TK_IDENTIFIER, s); }
    void emit_pp_number(const string & s) { add(TK_NUMBER, s); }
    void emit_character_literal(const string & s) { add(TK_CHARACTER, s); }
    void emit_user_defined_character_literal(const string & s) { add(TK_UD_CHARACTER, s); }
    void emit_string_literal(const string & s) { add(TK_STRING, s); }
    void emit_user_defined_string_literal(const string & s) { add(TK_UD_STRING, s); }
    void emit_preprocessing_op_or_punc(const string & s)
    { add(TK_PUNCT, s == "%:" ? "#" : (s == "%:%:" ? "##" : s)); }
    void emit_non_whitespace_char(const string & s) { add(TK_OTHER, s); }
    void emit_eof() {}
    void finish()
    {
      if (!current.empty())
      {
        physical_end_line = current.back().line;
        owner.process_line(*this);
        current.clear();
      }
      owner.emit_text(pending_text, presumed_file);
      if (!conditions.empty()) throw runtime_error("unterminated conditional group");
    }
  private:
    void add(TokenKind kind, const string & spelling)
    { current.push_back(Token(kind, spelling, line, column)); }
  };


  void process_file(const string & path, unsigned include_depth)
  {
    if (include_depth > 128) throw runtime_error("include nesting limit");
    ifstream in(path.c_str(), ios::binary);
    if (!in) throw runtime_error("cannot open source file");
    const string source((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    FileStream stream(*this, path, include_depth);
    PPTokenizer tokenizer(source, stream);
    tokenizer.tokenize();
    stream.finish();
  }
};

void Preprocessor::process_line(Preprocessor::FileStream & file)
  {
    vector<Token> & line = file.current;
    for (size_t i = 0; i < line.size(); ++i) locate(&line[i], file.presumed_file_ref, file.line_delta);
    size_t first = skip_space(line, 0);
    const bool is_directive = first < line.size() && is_punct(line[first], "#");
    if (!is_directive)
    {
      const bool enabled = file.conditions.empty() || file.conditions.back().active;
      if (enabled)
      {
        vector<Token> candidate = std::move(file.pending_text);
        for (size_t i = 0; i < line.size(); ++i)
          candidate.push_back(std::move(line[i]));
        candidate.push_back(Token(TK_SPACE));
        locate(&candidate.back(), file.presumed_file_ref, file.line_delta);
        vector<Token> deferred;
        emit_text(candidate, file.presumed_file, false, &deferred);
        file.pending_text = std::move(deferred);
      }
      return;
    }
    emit_text(file.pending_text, file.presumed_file);
    size_t name_at = skip_space(line, first + 1);
    if (name_at == line.size()) return; // null directive
    if (!is_identifier(line[name_at]))
    {
      if (file.conditions.empty() || file.conditions.back().active)
        throw runtime_error("non-directive in active region");
      return;
    }
    const string directive = line[name_at].text;
    const size_t args_at = name_at + 1;
    vector<Token> args(line.begin() + args_at, line.end());
    const bool enabled = file.conditions.empty() || file.conditions.back().active;
    if (directive == "if" || directive == "ifdef" || directive == "ifndef")
    {
      bool value = false;
      if (enabled)
      {
        if (directive == "if") value = evaluate_if(args);
        else
        {
          size_t a = skip_space(args, 0);
          if (a >= args.size() || !is_identifier(args[a])) throw runtime_error("invalid conditional identifier");
          const bool defined = macros_.find(args[a].text) != macros_.end();
          size_t tail = skip_space(args, a + 1);
          if (tail != args.size()) throw runtime_error("extra conditional tokens");
          value = directive == "ifdef" ? defined : !defined;
        }
      }
      file.conditions.push_back(Conditional(enabled, enabled && value));
      return;
    }
    if (directive == "elif")
    {
      if (file.conditions.empty()) throw runtime_error("unmatched #elif");
      Conditional & c = file.conditions.back();
      if (c.saw_else) throw runtime_error("#elif after #else");
      bool value = false;
      if (c.parent_active && !c.taken) value = evaluate_if(args);
      c.active = c.parent_active && !c.taken && value;
      if (c.active) c.taken = true;
      return;
    }
    if (directive == "else")
    {
      if (file.conditions.empty()) throw runtime_error("unmatched #else");
      Conditional & c = file.conditions.back();
      if (c.saw_else) throw runtime_error("duplicate #else");
      if (c.parent_active && skip_space(args, 0) != args.size()) throw runtime_error("extra tokens after #else");
      c.saw_else = true;
      c.active = c.parent_active && !c.taken;
      c.taken = true;
      return;
    }
    if (directive == "endif")
    {
      if (file.conditions.empty()) throw runtime_error("unmatched #endif");
      if (file.conditions.back().parent_active && skip_space(args, 0) != args.size()) throw runtime_error("extra tokens after #endif");
      file.conditions.pop_back();
      return;
    }
    if (!enabled) return;

    if (directive == "define") define_macro(line, skip_space(line, args_at));
    else if (directive == "undef")
    {
      size_t a = skip_space(args, 0);
      if (a >= args.size() || !is_identifier(args[a]) || args[a].text == "__VA_ARGS__" ||
          skip_space(args, a + 1) != args.size()) throw runtime_error("invalid #undef");
      macros_.erase(args[a].text);
    }
    else if (directive == "include")
    {
      const string header = include_path(file.presumed_file, std::move(args));
      PreprocessorFileId id;
      if (GetPreprocessorFileId(header, id) && once_.find(id) != once_.end()) return;
      process_file(header, file.include_depth + 1);
    }
    else if (directive == "line")
    {
      vector<Token> expanded = expand(std::move(args));
      size_t a = skip_space(expanded, 0);
      if (a >= expanded.size() || expanded[a].kind != TK_NUMBER) throw runtime_error("invalid #line number");
      char * tail = 0;
      unsigned long long value = strtoull(expanded[a].text.c_str(), &tail, 10);
      if (!tail || *tail || value == 0 || value > static_cast<unsigned long long>(numeric_limits<long long>::max()))
        throw runtime_error("invalid #line number");
      size_t b = skip_space(expanded, a + 1);
      const size_t after = skip_space(expanded, b + 1);
      if (b < expanded.size())
      {
        if (expanded[b].kind != TK_STRING || after != expanded.size()) throw runtime_error("invalid #line filename");
        string filename;
        if (!decode_string_literal(expanded[b].text, &filename)) throw runtime_error("invalid #line filename");
        file.presumed_file = filename;
        file.presumed_file_ref.reset(new string(filename));
      }
      else if (b != expanded.size()) throw runtime_error("invalid #line tokens");
      const size_t physical_next = file.physical_end_line + 1;
      file.line_delta = static_cast<long long>(value) - static_cast<long long>(physical_next);
    }
    else if (directive == "error") throw runtime_error("active #error");
    else if (directive == "pragma")
    {
      const size_t a = skip_space(args, 0);
      if (a < args.size() && is_identifier(args[a]) && args[a].text == "once")
        mark_pragma_once(file.presumed_file);
      // Unknown pragmas are deliberately ignored.
    }
    else throw runtime_error("non-directive in active region");
  }


} // anonymous namespace


void run_preprocessor_files(const std::vector<std::string> & sources,
                            IPPTokenStream & output,
                            const std::string & date_literal,
                            const std::string & time_literal)
{
  for (size_t i = 0; i < sources.size(); ++i)
  {
    Preprocessor preprocessor(date_literal, time_literal);
    preprocessor.begin_translation_unit(output);
    preprocessor.run_primary(sources[i]);
    output.emit_eof();
  }
}
