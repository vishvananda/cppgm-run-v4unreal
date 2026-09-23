// (C) 2013 CPPGM Foundation. All rights reserved.
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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
typedef uint32_t Paint;
struct PaintNode
{
  uint32_t identifier;
  unsigned priority;
  Paint left, right;
  size_t size;
  PaintNode(uint32_t id = 0, unsigned p = 0, Paint l = 0, Paint r = 0, size_t n = 0)
    : identifier(id), priority(p), left(l), right(r), size(n) {}
};

// Translation-unit-local persistent treap nodes are stored in a geometrically
// grown slab. Tokens carry compact root handles; no per-token shared ownership
// or per-node allocation is needed. The slab is reset after a completed logical
// line, unless an incomplete invocation keeps its painted tokens deferred.
class PaintTable
{
public:
  PaintTable() { reset(); }

  void reset()
  {
    nodes_.clear();
    nodes_.push_back(PaintNode()); // handle zero is the empty set
  }

  bool has(Paint root, uint32_t identifier) const
  {
    while (root)
    {
      const PaintNode & node = nodes_[root];
      if (identifier == node.identifier) return true;
      root = identifier < node.identifier ? node.left : node.right;
    }
    return false;
  }

  Paint insert(Paint root, uint32_t identifier)
  {
    if (has(root, identifier)) return root;
    return insert_missing(root, identifier);
  }

  Paint intersect(Paint a, Paint b)
  {
    if (a == b) return a;
    vector<uint32_t> identifiers;
    collect((a && (!b || nodes_[a].size <= nodes_[b].size)) ? a : b, &identifiers);
    const Paint other = (a && (!b || nodes_[a].size <= nodes_[b].size)) ? b : a;
    Paint result = 0;
    for (size_t i = 0; i < identifiers.size(); ++i)
      if (has(other, identifiers[i])) result = insert(result, identifiers[i]);
    return result;
  }

  Paint unite(Paint a, Paint b)
  {
    if (a == b) return a;
    vector<uint32_t> identifiers; collect(b, &identifiers);
    Paint result = a;
    for (size_t i = 0; i < identifiers.size(); ++i)
      result = insert(result, identifiers[i]);
    return result;
  }

  Paint difference(Paint a, Paint b)
  {
    if (!a || a == b) return 0;
    vector<uint32_t> identifiers; collect(a, &identifiers);
    Paint result = 0;
    for (size_t i = 0; i < identifiers.size(); ++i)
      if (!has(b, identifiers[i])) result = insert(result, identifiers[i]);
    return result;
  }

private:
  vector<PaintNode> nodes_;

  static unsigned priority(uint32_t identifier)
  {
    unsigned h = identifier + 0x9e3779b9u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
  }

  Paint make_node(uint32_t identifier, unsigned p, Paint left, Paint right)
  {
    if (nodes_.size() >= numeric_limits<uint32_t>::max())
      throw runtime_error("macro paint storage limit");
    const size_t size = 1 + (left ? nodes_[left].size : 0) + (right ? nodes_[right].size : 0);
    nodes_.push_back(PaintNode(identifier, p, left, right, size));
    return static_cast<Paint>(nodes_.size() - 1);
  }

  void split(Paint root, uint32_t key, Paint * left, Paint * right)
  {
    if (!root) { *left = *right = 0; return; }
    const PaintNode node = nodes_[root]; // recursive append may reallocate nodes_
    if (key < node.identifier)
    {
      Paint a, b; split(node.left, key, &a, &b);
      *left = a; *right = make_node(node.identifier, node.priority, b, node.right);
    }
    else
    {
      Paint a, b; split(node.right, key, &a, &b);
      *left = make_node(node.identifier, node.priority, node.left, a); *right = b;
    }
  }

  Paint insert_missing(Paint root, uint32_t identifier)
  {
    if (!root) return make_node(identifier, priority(identifier), 0, 0);
    const PaintNode node = nodes_[root];
    const unsigned p = priority(identifier);
    if (p < node.priority)
    {
      Paint left, right; split(root, identifier, &left, &right);
      return make_node(identifier, p, left, right);
    }
    if (identifier < node.identifier)
    {
      const Paint left = insert_missing(node.left, identifier);
      return make_node(node.identifier, node.priority, left, node.right);
    }
    const Paint right = insert_missing(node.right, identifier);
    return make_node(node.identifier, node.priority, node.left, right);
  }

  void collect(Paint root, vector<uint32_t> * out) const
  {
    if (!root) return;
    const PaintNode & node = nodes_[root];
    collect(node.left, out); out->push_back(node.identifier); collect(node.right, out);
  }
};

class IdentifierTable
{
public:
  IdentifierTable() : next_recent_(0), size_(0)
  {
    entries_.resize(16);
    for (size_t i = 0; i < 8; ++i) recent_[i].valid = false;
  }

  uint32_t intern(const string & spelling)
  {
    for (size_t i = 0; i < 8; ++i)
      if (recent_[i].valid && recent_[i].spelling == spelling) return recent_[i].id;
    size_t slot = find_slot(spelling);
    uint32_t id;
    if (entries_[slot].occupied) id = entries_[slot].id;
    else
    {
      if ((size_ + 1) * 10 >= entries_.size() * 7)
      {
        rehash(entries_.size() * 2);
        slot = find_slot(spelling);
      }
      id = static_cast<uint32_t>(names_.size());
      entries_[slot].spelling = spelling;
      entries_[slot].id = id;
      entries_[slot].occupied = true;
      names_.push_back(static_cast<uint32_t>(slot));
      ++size_;
    }
    recent_[next_recent_].spelling = spelling;
    recent_[next_recent_].id = id;
    recent_[next_recent_].valid = true;
    next_recent_ = (next_recent_ + 1) % 8;
    return id;
  }

  const string & spelling(uint32_t id) const
  {
    static const string empty;
    if (id >= names_.size()) return empty;
    return entries_[names_[id]].spelling;
  }

private:
  struct Entry {
    string spelling;
    uint32_t id;
    bool occupied;
    Entry() : id(0), occupied(false) {}
  };
  struct Recent { string spelling; uint32_t id; bool valid; };
  vector<Entry> entries_;
  vector<uint32_t> names_;
  Recent recent_[8];
  size_t next_recent_;
  size_t size_;

  size_t find_slot(const string & spelling) const
  {
    const size_t mask = entries_.size() - 1;
    size_t slot = hash<string>()(spelling) & mask;
    while (entries_[slot].occupied && entries_[slot].spelling != spelling)
      slot = (slot + 1) & mask;
    return slot;
  }

  void rehash(size_t capacity)
  {
    vector<Entry> old;
    old.swap(entries_);
    entries_.resize(capacity);
    for (size_t i = 0; i < old.size(); ++i)
    {
      if (!old[i].occupied) continue;
      const size_t slot = find_slot(old[i].spelling);
      entries_[slot].spelling = std::move(old[i].spelling);
      entries_[slot].id = old[i].id;
      entries_[slot].occupied = true;
      names_[entries_[slot].id] = static_cast<uint32_t>(slot);
    }
  }
};

struct Token
{
  TokenKind kind;
  string text_storage;
  const IdentifierTable * identifier_table;
  uint32_t identifier_id;
  uint32_t file_id;
  size_t line, column, presumed_line;
  Paint unavailable;
  Paint inherited_paint;
  bool from_macro;
  bool paste_operator, stringize_operator;
  Token(TokenKind k = TK_OTHER, const string & s = string(), size_t l = 1,
        size_t c = 1) : kind(k), text_storage(s), identifier_table(0),
                         identifier_id(0), file_id(0), line(l), column(c), presumed_line(l),
                         unavailable(0), inherited_paint(0), from_macro(false), paste_operator(false), stringize_operator(false) {}
  const string & spelling() const
  {
    return kind == TK_IDENTIFIER && identifier_table
      ? identifier_table->spelling(identifier_id) : text_storage;
  }
};

bool is_space(const Token & t) { return t.kind == TK_SPACE || t.kind == TK_NEWLINE; }
bool is_identifier(const Token & t) { return t.kind == TK_IDENTIFIER; }
bool is_punct(const Token & t, const string & s)
{
  return t.kind == TK_PUNCT && (t.spelling() == s ||
    (s == "#" && t.spelling() == "%:") || (s == "##" && t.spelling() == "%:%:"));
}
size_t skip_space(const vector<Token> & ts, size_t at)
{
  while (at < ts.size() && is_space(ts[at])) ++at;
  return at;
}

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
  vector<uint32_t> params;
  vector<Token> replacement;
  vector<uint32_t> replacement_parameter_index;
  Macro() : builtin(NONE), function_like(false), variadic(false) {}
};

class MacroTable
{
public:
  MacroTable() : size_(0), used_(0), deleted_(0) { slots_.resize(16); }

  void clear()
  {
    for (size_t i = 0; i < slots_.size(); ++i)
    {
      slots_[i].macro = Macro();
      slots_[i].state = EMPTY;
    }
    size_ = used_ = deleted_ = 0;
  }

  Macro * find(uint32_t key)
  {
    const size_t slot = find_existing(key);
    return slot == slots_.size() ? 0 : &slots_[slot].macro;
  }

  const Macro * find(uint32_t key) const
  {
    const size_t slot = find_existing(key);
    return slot == slots_.size() ? 0 : &slots_[slot].macro;
  }

  void set(uint32_t key, Macro macro)
  {
    if ((used_ + 1) * 10 >= slots_.size() * 7)
      rehash((size_ + 1) * 10 >= slots_.size() * 7
               ? slots_.size() * 2 : slots_.size());
    bool found = false;
    const size_t slot = find_insert(key, &found);
    Entry & entry = slots_[slot];
    if (!found)
    {
      if (entry.state == EMPTY) ++used_;
      else --deleted_;
      entry.key = key; entry.state = LIVE; ++size_;
    }
    entry.macro = std::move(macro);
  }

  bool erase(uint32_t key)
  {
    const size_t slot = find_existing(key);
    if (slot == slots_.size()) return false;
    slots_[slot].macro = Macro();
    slots_[slot].state = DELETED;
    --size_; ++deleted_;
    return true;
  }

private:
  enum State { EMPTY, LIVE, DELETED };
  struct Entry {
    uint32_t key;
    unsigned char state;
    Macro macro;
    Entry() : key(0), state(EMPTY) {}
  };
  vector<Entry> slots_;
  size_t size_, used_, deleted_;

  static size_t hash_key(uint32_t key)
  {
    unsigned h = key + 0x9e3779b9u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
  }

  size_t find_existing(uint32_t key) const
  {
    const size_t mask = slots_.size() - 1;
    size_t slot = hash_key(key) & mask;
    while (slots_[slot].state != EMPTY)
    {
      if (slots_[slot].state == LIVE && slots_[slot].key == key) return slot;
      slot = (slot + 1) & mask;
    }
    return slots_.size();
  }

  size_t find_insert(uint32_t key, bool * found) const
  {
    const size_t mask = slots_.size() - 1;
    size_t slot = hash_key(key) & mask;
    size_t first_deleted = slots_.size();
    for (;;)
    {
      const Entry & entry = slots_[slot];
      if (entry.state == EMPTY)
      {
        *found = false;
        return first_deleted == slots_.size() ? slot : first_deleted;
      }
      if (entry.state == LIVE && entry.key == key)
      {
        *found = true; return slot;
      }
      if (entry.state == DELETED && first_deleted == slots_.size())
        first_deleted = slot;
      slot = (slot + 1) & mask;
    }
  }

  void rehash(size_t capacity)
  {
    vector<Entry> old; old.swap(slots_);
    slots_.resize(capacity);
    size_ = used_ = deleted_ = 0;
    for (size_t i = 0; i < old.size(); ++i)
    {
      if (old[i].state != LIVE) continue;
      bool found = false;
      const size_t slot = find_insert(old[i].key, &found);
      slots_[slot].key = old[i].key;
      slots_[slot].state = LIVE;
      slots_[slot].macro = std::move(old[i].macro);
      ++size_; ++used_;
    }
  }
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
    : date_(date), time_(time), va_args_id_(0), pragma_id_(0), defined_id_(0),
      true_id_(0), false_id_(0), once_id_(0), has_cpp_attribute_id_(0),
      no_unique_address_id_(0), no_unique_address_alt_id_(0),
      counter_(0), post_(0) {}

  void begin_translation_unit(IPPTokenStream & output)
  {
    macros_.clear();
    once_.clear();
    counter_ = 0;
    post_ = &output;
    va_args_id_ = identifiers_.intern("__VA_ARGS__");
    pragma_id_ = identifiers_.intern("_Pragma");
    defined_id_ = identifiers_.intern("defined");
    true_id_ = identifiers_.intern("true");
    false_id_ = identifiers_.intern("false");
    once_id_ = identifiers_.intern("once");
    has_cpp_attribute_id_ = identifiers_.intern("__has_cpp_attribute");
    no_unique_address_id_ = identifiers_.intern("no_unique_address");
    no_unique_address_alt_id_ = identifiers_.intern("__no_unique_address__");
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
    macros_.set(has_cpp_attribute_id_, std::move(attr));
  }

  void run_primary(const string & path)
  {
    process_file(path, 0);
  }

  uint32_t intern_file(const string & path)
  {
    unordered_map<string, uint32_t>::const_iterator found = file_ids_.find(path);
    if (found != file_ids_.end()) return found->second;
    if (file_names_.size() >= numeric_limits<uint32_t>::max())
      throw runtime_error("source file identity limit");
    const uint32_t id = static_cast<uint32_t>(file_names_.size());
    file_names_.push_back(path);
    file_ids_[path] = id;
    return id;
  }

  const string & file_name(uint32_t id) const
  {
    static const string empty;
    return id < file_names_.size() ? file_names_[id] : empty;
  }

private:
  string date_, time_;
  MacroTable macros_;
  IdentifierTable identifiers_;
  PaintTable paints_;
  uint32_t va_args_id_, pragma_id_, defined_id_, true_id_, false_id_, once_id_;
  uint32_t has_cpp_attribute_id_, no_unique_address_id_, no_unique_address_alt_id_;
  unordered_set<PreprocessorFileId, FileIdHash> once_;
  deque<string> file_names_;
  unordered_map<string, uint32_t> file_ids_;
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
    macros_.set(identifiers_.intern(name), std::move(m));
  }

  void locate(Token * t, uint32_t file_id, long long line_delta)
  {
    t->file_id = file_id;
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
    for (size_t i = 0; i < a.params.size(); ++i)
      if (a.params[i] != b.params[i]) return false;
    for (size_t i = 0; i < a.replacement.size(); ++i)
    {
      const Token & x = a.replacement[i], &y = b.replacement[i];
      if (x.kind != y.kind) return false;
      if (x.kind == TK_IDENTIFIER)
      {
        if (x.identifier_id != y.identifier_id) return false;
      }
      else if (x.spelling() != y.spelling()) return false;
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


  void define_macro(const vector<Token> & line, size_t at)
  {
    if (at >= line.size() || !is_identifier(line[at])) throw runtime_error("invalid #define");
    const uint32_t name_id = line[at].identifier_id;
    const string name = line[at].spelling();
    if (name_id == va_args_id_) throw runtime_error("invalid macro name");
    ++at;
    Macro m;
    if (at < line.size() && line[at].kind == TK_PUNCT && line[at].spelling() == "(")
    {
      m.function_like = true;
      ++at;
      at = skip_space(line, at);
      if (at < line.size() && line[at].kind == TK_PUNCT && line[at].spelling() == ")")
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
            if (at >= line.size() || line[at].kind != TK_PUNCT || line[at].spelling() != ")")
              throw runtime_error("variadic parameter must be last");
            ++at; break;
          }
          if (!is_identifier(line[at]) || line[at].spelling() == "__VA_ARGS__")
            throw runtime_error("invalid macro parameter");
          const uint32_t param = line[at++].identifier_id;
          if (find(m.params.begin(), m.params.end(), param) != m.params.end())
            throw runtime_error("duplicate macro parameter");
          m.params.push_back(param); need_param = false;
          at = skip_space(line, at);
          if (at >= line.size()) throw runtime_error("unterminated macro parameter list");
          if (line[at].kind == TK_PUNCT && line[at].spelling() == ")") { ++at; break; }
          if (line[at].kind == TK_PUNCT && line[at].spelling() == ",") { ++at; need_param = true; continue; }
          throw runtime_error("invalid macro parameter list");
        }
      }
    }
    if (m.function_like && m.variadic) { /* __VA_ARGS__ is an implicit tail parameter */ }
    vector<Token> raw(line.begin() + at, line.end());
    m.replacement = normalize_replacement(raw);
    for (size_t i = 0; i < m.replacement.size(); ++i)
    {
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].spelling() == "##") m.replacement[i].paste_operator = true;
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].spelling() == "#") m.replacement[i].stringize_operator = true;
    }
    unordered_map<uint32_t, size_t> param_map;
    for (size_t i = 0; i < m.params.size(); ++i) param_map[m.params[i]] = i;
    const uint32_t no_parameter = numeric_limits<uint32_t>::max();
    m.replacement_parameter_index.assign(m.replacement.size(), no_parameter);
    for (size_t i = 0; i < m.replacement.size(); ++i)
    {
      if (is_identifier(m.replacement[i]))
      {
        unordered_map<uint32_t, size_t>::const_iterator parameter =
          param_map.find(m.replacement[i].identifier_id);
        if (parameter != param_map.end())
          m.replacement_parameter_index[i] = static_cast<uint32_t>(parameter->second);
        else if (m.variadic && m.replacement[i].identifier_id == va_args_id_)
          m.replacement_parameter_index[i] = static_cast<uint32_t>(m.params.size());
      }
      if (m.replacement[i].kind == TK_IDENTIFIER && m.replacement[i].identifier_id == va_args_id_ && !m.variadic)
        throw runtime_error("__VA_ARGS__ used outside variadic macro");
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].spelling() == "#")
      {
        size_t before = i; while (before && is_space(m.replacement[before - 1])) --before;
        size_t after = i + 1; while (after < m.replacement.size() && is_space(m.replacement[after])) ++after;
        const bool paste_operand = (before && m.replacement[before - 1].kind == TK_PUNCT && m.replacement[before - 1].spelling() == "##") ||
          (after < m.replacement.size() && m.replacement[after].kind == TK_PUNCT && m.replacement[after].spelling() == "##");
        if (!paste_operand)
        {
          if (!m.function_like || after >= m.replacement.size() ||
              !(is_identifier(m.replacement[after]) && (param_map.count(m.replacement[after].identifier_id) ||
                (m.variadic && m.replacement[after].identifier_id == va_args_id_))))
            throw runtime_error("invalid stringizing operator");
        }
      }
      if (m.replacement[i].kind == TK_PUNCT && m.replacement[i].spelling() == "##")
      {
        size_t left = i;
        while (left && is_space(m.replacement[left - 1])) --left;
        size_t right = i + 1;
        while (right < m.replacement.size() && is_space(m.replacement[right])) ++right;
        if (left == 0 || right == m.replacement.size())
          throw runtime_error("invalid token paste operator");
      }
    }
    Macro * found = macros_.find(name_id);
    if (found)
    {
      if (!same_definition(*found, m)) throw runtime_error("incompatible macro redefinition");
      return;
    }
    macros_.set(name_id, std::move(m));
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
      if (t.kind == TK_PUNCT && t.spelling() == "(") { ++depth; current.push_back(t); saw_any = true; }
      else if (t.kind == TK_PUNCT && t.spelling() == ")")
      {
        if (depth == 0)
        {
          if (saw_any || !current.empty()) args->push_back(current);
          *consumed = i + 1;
          return true;
        }
        --depth; current.push_back(t); saw_any = true;
      }
      else if (t.kind == TK_PUNCT && t.spelling() == "," && depth == 0)
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
    const string combined = left.spelling() + right.spelling();
    vector<Token> lexed = tokenize(combined);
    vector<Token> significant;
    for (size_t i = 0; i < lexed.size(); ++i)
      if (!is_space(lexed[i])) significant.push_back(lexed[i]);
    if (significant.size() != 1 || significant[0].kind == TK_OTHER ||
        significant[0].kind == TK_HEADER)
      throw runtime_error(string("invalid token paste in ") + macro_name + " [" + combined + "]");
    Token result = significant[0];
    if (result.kind == TK_IDENTIFIER)
    {
      const string pasted_spelling = result.spelling();
      result.identifier_id = identifiers_.intern(pasted_spelling);
      result.identifier_table = &identifiers_;
      result.text_storage.clear();
    }
    result.file_id = head.file_id; result.line = head.line; result.presumed_line = head.presumed_line;
    result.unavailable = paints_.unite(left.unavailable, right.unavailable);
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
      if (macro.variadic && left.kind == TK_PUNCT && left.spelling() == ",")
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

  BuiltinResult expand_builtin(Token & head, uint32_t macro_id,
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
      if (open >= work.size() || work[open].kind != TK_PUNCT || work[open].spelling() != "(")
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
            (builtin_args[0][ai].identifier_id == no_unique_address_id_ ||
             builtin_args[0][ai].identifier_id == no_unique_address_alt_id_))
          known_attribute = true;
      }
    }
    Token replacement;
    if (macro.builtin == Macro::LINE)
      replacement = Token(TK_NUMBER, to_string(head.presumed_line), head.line, head.column);
    else if (macro.builtin == Macro::FILE)
      replacement = Token(TK_STRING, quote_string(file_name(head.file_id)), head.line, head.column);
    else if (macro.builtin == Macro::DATE)
      replacement = Token(TK_STRING, macro.replacement.empty() ? date_ : macro.replacement[0].spelling(), head.line, head.column);
    else if (macro.builtin == Macro::TIME)
      replacement = Token(TK_STRING, macro.replacement.empty() ? time_ : macro.replacement[0].spelling(), head.line, head.column);
    else if (macro.builtin == Macro::COUNTER)
      replacement = Token(TK_NUMBER, to_string(counter_++), head.line, head.column);
    else
      replacement = Token(TK_NUMBER, known_attribute ? "201803" : "0", head.line, head.column);
    replacement.file_id = head.file_id; replacement.presumed_line = head.presumed_line;
    replacement.unavailable = paints_.insert(head.unavailable, macro_id);
    replacement.inherited_paint = head.unavailable; replacement.from_macro = true;
    work.push_front(std::move(replacement));
    return BUILTIN_REPLACED;
  }

  vector<Token> expand(vector<Token> input, unsigned depth = 0,
                       bool * contains_pragma_operator = 0, bool final = true,
                       vector<Token> * deferred = 0, size_t * shared_work = 0)
  {
    if (depth > 256) throw runtime_error("macro argument expansion nesting limit");
    size_t local_work = 0;
    if (!shared_work) shared_work = &local_work;
    bool possible_macro = false, found_pragma_operator = false;
    for (size_t i = 0; i < input.size(); ++i)
    {
      if (is_identifier(input[i]) && input[i].identifier_id == pragma_id_) found_pragma_operator = true;
      if (is_identifier(input[i]) && !paints_.has(input[i].unavailable, input[i].identifier_id) &&
          macros_.find(input[i].identifier_id)) possible_macro = true;
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
    while (!work.empty())
    {
      if (++*shared_work > 10000000) throw runtime_error("macro expansion work limit");
      Token head = std::move(work.front()); work.pop_front();
      if (!is_identifier(head) || paints_.has(head.unavailable, head.identifier_id))
      { output.push_back(std::move(head)); continue; }
      if (!final && head.identifier_id == pragma_id_)
      {
        if (deferred) deferred->push_back(std::move(head));
        while (!work.empty()) { if (deferred) deferred->push_back(std::move(work.front())); work.pop_front(); }
        break;
      }
      Macro * found = macros_.find(head.identifier_id);
      if (!found) { output.push_back(std::move(head)); continue; }
      const uint32_t macro_id = head.identifier_id;
      const string name = head.spelling();
      const Macro & macro = *found;

      if (macro.builtin != Macro::NONE)
      {
        const BuiltinResult result = expand_builtin(head, macro_id, macro, work, final, deferred);
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
        if (open >= work.size() || work[open].kind != TK_PUNCT || work[open].spelling() != "(")
        { output.push_back(std::move(head)); continue; }
        if (!parse_arguments(work, open, &args, &consumed))
        {
          if (final) throw runtime_error("unterminated function macro invocation");
          work.push_front(std::move(head));
          while (!work.empty()) { if (deferred) deferred->push_back(std::move(work.front())); work.pop_front(); }
          break;
        }
        const Paint & closing_paint = work[consumed - 1].unavailable;
        macro_paint = paints_.intersect(head.unavailable, closing_paint);
        macro_paint = paints_.insert(macro_paint, macro_id);
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
        const uint32_t no_parameter = numeric_limits<uint32_t>::max();
        for (size_t ri = 0; ri < macro.replacement.size(); ++ri)
        {
          const Token & rt = macro.replacement[ri];
          size_t hash_after = ri + 1;
          while (hash_after < macro.replacement.size() && is_space(macro.replacement[hash_after])) ++hash_after;
          size_t hash_before = ri; while (hash_before && is_space(macro.replacement[hash_before - 1])) --hash_before;
          const bool stringize_candidate = rt.stringize_operator &&
            hash_after < macro.replacement.size() && is_identifier(macro.replacement[hash_after]) &&
            macro.replacement_parameter_index[hash_after] != no_parameter;
          const bool hash_is_paste = rt.kind == TK_PUNCT && rt.spelling() == "#" && !stringize_candidate &&
            ((hash_after < macro.replacement.size() && macro.replacement[hash_after].kind == TK_PUNCT && macro.replacement[hash_after].spelling() == "##") ||
             (hash_before && macro.replacement[hash_before - 1].kind == TK_PUNCT && macro.replacement[hash_before - 1].spelling() == "##"));
          if (stringize_candidate && !hash_is_paste)
          {
            size_t pi = skip_space(macro.replacement, ri + 1);
            const size_t ix = macro.replacement_parameter_index[pi];
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
              for (size_t z = 0; z < raw[q].spelling().size(); ++z)
              {
                const bool literal_token = raw[q].kind == TK_STRING || raw[q].kind == TK_UD_STRING ||
                  raw[q].kind == TK_CHARACTER || raw[q].kind == TK_UD_CHARACTER;
                if (literal_token && (raw[q].spelling()[z] == '\\' || raw[q].spelling()[z] == '"'))
                  content.push_back('\\');
                content.push_back(raw[q].spelling()[z]);
              }
            }
            Token stringized(TK_STRING, string("\"") + content + "\"", head.line, head.column);
            stringized.file_id = head.file_id; stringized.presumed_line = head.presumed_line;
            stringized.unavailable = macro_paint;
            stringized.inherited_paint = head.unavailable; stringized.from_macro = true;
            substituted.push_back(stringized); ri = pi;
            continue;
          }
          const uint32_t parameter_index = macro.replacement_parameter_index[ri];
          if (is_identifier(rt) && parameter_index != no_parameter)
          {
            const size_t ix = parameter_index;
            size_t prev = ri; while (prev && is_space(macro.replacement[prev - 1])) --prev;
            size_t next = ri + 1; while (next < macro.replacement.size() && is_space(macro.replacement[next])) ++next;
            const bool pasted = (prev && macro.replacement[prev - 1].kind == TK_PUNCT && macro.replacement[prev - 1].spelling() == "##") ||
              (next < macro.replacement.size() && macro.replacement[next].kind == TK_PUNCT && macro.replacement[next].spelling() == "##");
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
              if (!expanded_ready[ix])
              {
                expanded_args[ix] = expand(std::move(values), depth + 1, 0, true, 0, shared_work);
                expanded_ready[ix] = true;
              }
              values = expanded_args[ix];
            }
            for (size_t q = 0; q < values.size(); ++q)
            {
              Token value = values[q];
              if (is_identifier(value) && macros_.find(value.identifier_id) != 0)
              {
                if (value.from_macro)
                {
                  value.unavailable = paints_.difference(value.unavailable, value.inherited_paint);
                  Paint current = paints_.insert(0, macro_id);
                  value.unavailable = paints_.unite(value.unavailable, current);
                }
                else
                {
                  value.unavailable = paints_.unite(value.unavailable, head.unavailable);
                  value.unavailable = paints_.unite(value.unavailable, macro_paint);
                }
                value.inherited_paint = head.unavailable;
                value.from_macro = true;
              }
              substituted.push_back(value);
            }
            continue;
          }
          Token copy = rt;
          copy.file_id = head.file_id; copy.line = head.line; copy.column = head.column;
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
          body[q].file_id = head.file_id; body[q].line = head.line; body[q].column = head.column;
          body[q].presumed_line = head.presumed_line;
          body[q].unavailable = paints_.insert(head.unavailable, macro_id);
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
          if (is_identifier(output[i]) && output[i].identifier_id == pragma_id_) { found_pragma_operator = true; break; }
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
      if (is_identifier(expression[i]) && expression[i].identifier_id == defined_id_)
      {
        size_t j = skip_space(expression, i + 1); bool paren = false;
        if (j < expression.size() && expression[j].kind == TK_PUNCT && expression[j].spelling() == "(")
        { paren = true; j = skip_space(expression, j + 1); }
        if (j >= expression.size() || !(is_identifier(expression[j]) ||
            (expression[j].kind == TK_PUNCT && is_alternative_word(expression[j].spelling()))))
          throw runtime_error("invalid defined operator");
        const string operand = expression[j].spelling();
        const bool exists = macros_.find(expression[j].identifier_id) != 0;
        ++j; j = skip_space(expression, j);
        if (paren)
        {
          if (j >= expression.size() || expression[j].kind != TK_PUNCT || expression[j].spelling() != ")")
            throw runtime_error("invalid defined operator");
          ++j;
        }
        Token value(TK_NUMBER, exists ? "1" : "0", expression[i].line, expression[i].column);
        value.file_id = expression[i].file_id; value.presumed_line = expression[i].presumed_line;
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
        if (t.identifier_id == true_id_ || t.identifier_id == false_id_)
          operands.push_back(ControlExpressionToken(ControlExpressionToken::IDENTIFIER, t.spelling()));
        else operands.push_back(ControlExpressionToken(ControlExpressionToken::PP_NUMBER, "0"));
      }
      else if (t.kind == TK_NUMBER)
        operands.push_back(ControlExpressionToken(ControlExpressionToken::PP_NUMBER, t.spelling()));
      else if (t.kind == TK_CHARACTER)
        operands.push_back(ControlExpressionToken(ControlExpressionToken::CHARACTER_LITERAL, t.spelling()));
      else if (t.kind == TK_PUNCT)
        operands.push_back(ControlExpressionToken(ControlExpressionToken::OPERATOR, t.spelling()));
      else operands.push_back(ControlExpressionToken(ControlExpressionToken::INVALID, t.spelling()));
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
      if (is_identifier((*tokens)[i]) && (*tokens)[i].identifier_id == pragma_id_) { has_pragma_operator = true; break; }
    if (!has_pragma_operator) return;
    vector<Token> out;
    for (size_t i = 0; i < tokens->size(); ++i)
    {
      const Token & t = (*tokens)[i];
      if (!is_identifier(t) || t.identifier_id != pragma_id_) { out.push_back(t); continue; }
      size_t j = skip_space(*tokens, i + 1);
      if (j >= tokens->size() || (*tokens)[j].kind != TK_PUNCT || (*tokens)[j].spelling() != "(")
        throw runtime_error("invalid _Pragma operator");
      j = skip_space(*tokens, j + 1);
      if (j >= tokens->size() || ((*tokens)[j].kind != TK_STRING && (*tokens)[j].kind != TK_UD_STRING))
        throw runtime_error("invalid _Pragma operand");
      string pragma;
      if (!decode_string_literal((*tokens)[j].spelling(), &pragma)) throw runtime_error("invalid _Pragma string");
      size_t end = skip_space(*tokens, j + 1);
      if (end >= tokens->size() || (*tokens)[end].kind != TK_PUNCT || (*tokens)[end].spelling() != ")")
        throw runtime_error("invalid _Pragma invocation");
      if (pragma == "once") mark_pragma_once(t.file_id < file_names_.size() ? file_name(t.file_id) : fallback_file);
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
      post_->set_source_file(file_name(t.file_id));
      post_->set_source_location(t.presumed_line, t.column);
      switch (t.kind)
      {
        case TK_SPACE: case TK_NEWLINE: post_->emit_whitespace_sequence(); break;
        case TK_HEADER: post_->emit_header_name(t.spelling()); break;
        case TK_IDENTIFIER:
          if (t.identifier_id == va_args_id_) throw runtime_error("__VA_ARGS__ outside variadic macro");
          post_->emit_identifier(t.spelling()); break;
        case TK_NUMBER: post_->emit_pp_number(t.spelling()); break;
        case TK_CHARACTER: post_->emit_character_literal(t.spelling()); break;
        case TK_UD_CHARACTER: post_->emit_user_defined_character_literal(t.spelling()); break;
        case TK_STRING: post_->emit_string_literal(t.spelling()); break;
        case TK_UD_STRING: post_->emit_user_defined_string_literal(t.spelling()); break;
        case TK_PUNCT: post_->emit_preprocessing_op_or_punc(t.spelling()); break;
        case TK_OTHER: post_->emit_non_whitespace_char(t.spelling()); break;
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
      const string & h = expanded[start].spelling();
      if (h.size() < 2 || (h[0] != '<' && h[0] != '"')) throw runtime_error("invalid header name");
      next = h.substr(1, h.size() - 2);
    }
    else if (end == start + 1 && expanded[start].kind == TK_STRING)
    {
      if (!decode_string_literal(expanded[start].spelling(), &next)) throw runtime_error("invalid include string");
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
  void process_line_contents(FileStream &);

  struct FileStream : IPPTokenStream
  {
    Preprocessor & owner;
    unsigned include_depth;
    string presumed_file;
    uint32_t file_id;
    long long line_delta;
    size_t line, column, physical_end_line;
    vector<Token> current, pending_text;
    vector<Conditional> conditions;

    FileStream(Preprocessor & p, const string & path, unsigned depth)
      : owner(p), include_depth(depth), presumed_file(path),
        file_id(p.intern_file(path)), line_delta(0),
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
      pending_text.clear();
      owner.paints_.reset();
      if (!conditions.empty()) throw runtime_error("unterminated conditional group");
    }
  private:
    void add(TokenKind kind, const string & spelling)
    {
      Token token(kind, string(), line, column);
      if (kind == TK_IDENTIFIER)
      {
        token.identifier_table = &owner.identifiers_;
        token.identifier_id = owner.identifiers_.intern(spelling);
      }
      else token.text_storage = spelling;
      current.push_back(std::move(token));
    }
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
  process_line_contents(file);
  if (file.pending_text.empty()) paints_.reset();
}

void Preprocessor::process_line_contents(Preprocessor::FileStream & file)
  {
    vector<Token> & line = file.current;
    for (size_t i = 0; i < line.size(); ++i) locate(&line[i], file.file_id, file.line_delta);
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
        locate(&candidate.back(), file.file_id, file.line_delta);
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
    const string directive = line[name_at].spelling();
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
          const bool defined = macros_.find(args[a].identifier_id) != 0;
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
      if (a >= args.size() || !is_identifier(args[a]) || args[a].identifier_id == va_args_id_ ||
          skip_space(args, a + 1) != args.size()) throw runtime_error("invalid #undef");
      macros_.erase(args[a].identifier_id);
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
      unsigned long long value = strtoull(expanded[a].spelling().c_str(), &tail, 10);
      if (!tail || *tail || value == 0 || value > static_cast<unsigned long long>(numeric_limits<long long>::max()))
        throw runtime_error("invalid #line number");
      size_t b = skip_space(expanded, a + 1);
      const size_t after = skip_space(expanded, b + 1);
      if (b < expanded.size())
      {
        if (expanded[b].kind != TK_STRING || after != expanded.size()) throw runtime_error("invalid #line filename");
        string filename;
        if (!decode_string_literal(expanded[b].spelling(), &filename)) throw runtime_error("invalid #line filename");
        file.presumed_file = filename;
        file.file_id = intern_file(filename);
      }
      else if (b != expanded.size()) throw runtime_error("invalid #line tokens");
      const size_t physical_next = file.physical_end_line + 1;
      file.line_delta = static_cast<long long>(value) - static_cast<long long>(physical_next);
    }
    else if (directive == "error") throw runtime_error("active #error");
    else if (directive == "pragma")
    {
      const size_t a = skip_space(args, 0);
      if (a < args.size() && is_identifier(args[a]) && args[a].identifier_id == once_id_)
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
