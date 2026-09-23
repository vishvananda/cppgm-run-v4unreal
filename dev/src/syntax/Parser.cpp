#include "syntax/Parser.h"
#include "syntax/FlatNameSet.h"
#include "syntax/TokenLabels.h"
#include "syntax/TokenView.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
using Node = SyntaxNode;
class Parser {
  // Speculative declarations and function parameters live in explicit lexical
  // frames.  Pop them on both the normal and exceptional parse paths.
  class ScopeGuard {
    Parser &parser_;
  public:
    explicit ScopeGuard(Parser &parser) : parser_(parser) { parser_.push_scope(); }
    ~ScopeGuard() { parser_.pop_scope(); }
  private:
    ScopeGuard(const ScopeGuard &);
    ScopeGuard &operator=(const ScopeGuard &);
  };

  SyntaxArena &arena_;
  TokenView t_;
  std::size_t p_;
  std::size_t template_lookahead_work_;
  std::size_t template_lookahead_budget_;
  PostTokenBuffer &tokens_;
  FlatNameSet qualified_types_, qualified_templates_;
  std::vector<FlatNameSet> types_, values_, templates_;
  std::vector<std::string> class_names_;
  unsigned force_type_names_;
  unsigned active_template_declarations_;
  bool semantic_mode_;
  std::vector<std::size_t> brace_mate_;
  Tok tok(std::size_t k = 0) const {
    static const std::string eof_spelling("<eof>");
    return p_ + k < t_.size() ? t_[p_ + k]
                              : Tok(eof_spelling, PostTokenEof);
  }
  std::string s(std::size_t k = 0) const { return tok(k).s; }
  bool at(const std::string &x) const { return s() == x; }
  bool take(const std::string &x) {
    if (at(x)) {
      ++p_;
      return true;
    }
    return false;
  }
  bool identifier(std::size_t k = 0) const {
    return tok(k).kind == PostTokenIdentifier;
  }
  bool literal(std::size_t k = 0) const {
    return tok(k).kind == PostTokenLiteral ||
           tok(k).kind == PostTokenUserDefinedLiteral;
  }
  bool eof() const { return p_ >= t_.size(); }
  Node n(const std::string &text) const {
    Node result(text, &arena_);
    if (p_ < t_.size())
      result.location = t_[p_].location;
    else if (p_ > 0)
      result.location = t_[p_ - 1].location;
    return result;
  }
  void need(const std::string &x) {
    if (!take(x))
      throw std::runtime_error("expected " + x);
  }
  std::string label(std::size_t k = 0) const {
    return syntax_token_label(s(k));
  }
  bool is_kw(const std::string &x) const { return syntax_is_keyword(x); }
  bool is_op(const std::string &x) const { return syntax_is_operator(x); }
  bool known_type(const std::string &x) const {
    const std::size_t qualifier = x.rfind("::");
    if (qualifier != std::string::npos) {
      std::string member = x.substr(qualifier + 2);
      const std::size_t angle = member.find('<');
      if (angle != std::string::npos) member.erase(angle);
      if (qualified_types_.count(member)) return true;
      return known_type(member);
    }
    for (std::size_t i = types_.size(); i > 0; --i) {
      const std::size_t k = i - 1;
      // The nearest declaration wins across categories. A parameter/local
      // value must hide an outer class or template name.
      if (values_[k].count(x))
        return false;
      if (types_[k].count(x))
        return true;
    }
    return category_is_type(x);
  }
  bool known_value(const std::string &x) const {
    for (std::size_t i = values_.size(); i > 0; --i) {
      const std::size_t k = i - 1;
      if (values_[k].count(x))
        return true;
      if (types_[k].count(x) || templates_[k].count(x))
        return false;
    }
    return false;
  }
  bool category_is_type(const std::string &x) const {
    for (std::size_t i = types_.size(); i > 0; --i) {
      const std::size_t k = i - 1;
      if (values_[k].count(x))
        return false;
      if (types_[k].count(x))
        return true;
    }
    if (!class_names_.empty() && qualified_types_.count(x)) return true;
    return x.find('C') != std::string::npos ||
           x.find('Y') != std::string::npos ||
           x.find('E') != std::string::npos || x.find('T') != std::string::npos;
  }
  bool known_template(const std::string &x) const {
    const std::size_t qualifier = x.rfind("::");
    if (qualifier != std::string::npos) {
      std::string member = x.substr(qualifier + 2);
      const std::size_t angle = member.find('<');
      if (angle != std::string::npos) member.erase(angle);
      if (qualified_templates_.count(member)) return true;
      return known_template(member);
    }
    for (std::size_t i = templates_.size(); i > 0; --i) {
      const std::size_t k = i - 1;
      if (templates_[k].count(x))
        return true;
      if (values_[k].count(x) || types_[k].count(x))
        return false;
    }
    if (!class_names_.empty() && qualified_templates_.count(x)) return true;
    return x.find('T') != std::string::npos && !known_value(x);
  }
  bool definite_type_start(std::size_t k = 0) const {
    const std::string x = s(k);
    if (x == "typename" || x == "decltype" || x == "class" || x == "struct" ||
        x == "union" || x == "enum" || x == "nullptr_t")
      return true;
    if (syntax_is_keyword(x) &&
        (x == "bool" || x == "char" || x == "char16_t" || x == "char32_t" ||
         x == "double" || x == "float" || x == "int" || x == "long" ||
         x == "short" || x == "signed" || x == "unsigned" || x == "void" ||
         x == "wchar_t" || x == "const" || x == "volatile" || x == "auto" || x == "nullptr_t"))
      return true;
    if (x == "::") {
      std::size_t j = k + 1;
      while (j + 1 < t_.size() && t_[j + 1].s == "::")
        j += 2;
      return qualified_types_.count(s(j)) || category_is_type(s(j));
    }
    if (identifier(k)) {
      if (s(k + 1) == "::") {
        std::size_t j = k + 2;
        while (j + 1 < t_.size() && t_[j + 1].s == "::")
          j += 2;
        if (qualified_types_.count(s(j)) || category_is_type(s(j))) return true;
        // Leave the final type/value distinction to PA6 semantic lookup. A
        // following declarator token makes this a type-shaped qualified-id.
        if(!semantic_mode_)return false;
        const std::string &next=s(j+1);
        return identifier(j+1) || next=="*" || next=="&" || next=="&&" || next=="(";
      }
      return category_is_type(x);
    }
    return false;
  }
  bool type_start(std::size_t k = 0) const {
    const std::string x = s(k);
    static const char *b[] = {
        "bool",    "char",  "char16_t", "char32_t", "double",   "float",
        "int",     "long",  "short",    "signed",   "unsigned", "void",
        "wchar_t", "const", "volatile", "typename", "decltype", "class",
        "struct",  "union", "enum",     "auto", "nullptr_t"};
    for (std::size_t i = 0; i < sizeof(b) / sizeof(*b); ++i)
      if (x == b[i])
        return true;
    if (x == "::")
      return true;
    if (identifier(k))
      return s(k + 1) == "::" ? definite_type_start(k) : category_is_type(x);
    return false;
  }
  void index_braces() {
    brace_mate_.assign(t_.size(),t_.size());
    std::vector<std::size_t> stack;
    for(std::size_t i=0;i<t_.size();++i) {
      if(t_[i].s=="{") stack.push_back(i);
      else if(t_[i].s=="}" && !stack.empty()) {
        const std::size_t open=stack.back(); stack.pop_back();
        brace_mate_[open]=i; brace_mate_[i]=open;
      }
    }
  }
  void push_scope() {
    types_.push_back(FlatNameSet(tokens_));
    values_.push_back(FlatNameSet(tokens_));
    templates_.push_back(FlatNameSet(tokens_));
  }
  void pop_scope() {
    if (types_.size() > 1) {
      types_.pop_back();
      values_.pop_back();
      templates_.pop_back();
    }
  }
  void declare_type(const std::string &x) {
    if (!x.empty())
      types_.back().insert(x);
  }
  void declare_value(const std::string &x) {
    if (!x.empty()) {
      values_.back().insert(x);
      types_.back().erase(x);
      templates_.back().erase(x);
    }
  }
  static bool has_hint(const std::string &x, char c) {
    return x.find(c) != std::string::npos;
  }
  std::string identifier_text() {
    if (!identifier())
      throw std::runtime_error("identifier expected");
    return t_[p_++].s;
  }
  std::string compact_range(std::size_t a, std::size_t b) const {
    std::string r;
    for (std::size_t i = a; i < b; ++i) {
      if (i > a) {
        const std::string &prev = t_[i - 1].s, &cur = t_[i].s;
        bool prevword =
            !prev.empty() &&
            (std::isalnum(static_cast<unsigned char>(prev[prev.size() - 1])) ||
             prev[prev.size() - 1] == '_');
        bool curword =
            !cur.empty() &&
            (std::isalnum(static_cast<unsigned char>(cur[0])) || cur[0] == '_');
        if (prevword && curword && prev != "operator")
          r.push_back(' ');
      }
      r += t_[i].s;
    }
    return r;
  }
  bool template_candidate(const std::string &name) {
    if (known_template(name))
      return true;
    if (known_value(name) || !at("<"))
      return false;
    int depth = 0, par = 0, br = 0, brace = 0;
    for (std::size_t i = p_; i < t_.size(); ++i) {
      if (template_lookahead_work_ >= template_lookahead_budget_)
        return false;
      ++template_lookahead_work_;
      const std::string &x = t_[i].s;
      if (x == "(")
        ++par;
      else if (x == ")") {
        if (!par)
          break;
        --par;
      } else if (x == "[")
        ++br;
      else if (x == "]") {
        if (!br)
          break;
        --br;
      } else if (x == "{") {
        if (!par && !br && !brace)
          break;
        ++brace;
      }
      else if (x == "}") {
        if (!brace)
          break;
        --brace;
      }
      if (!par && !br && !brace) {
        // An unmatched relational '<' must not make every later name in the
        // translation unit rescan the remaining token stream. A top-level
        // semicolon/body boundary cannot belong to this template-id.
        if (x == ";" || x == "{")
          break;
        if (x == "<")
          ++depth;
        else if (x == ">") {
          if (--depth == 0) {
            if (i + 1 >= t_.size())
              return true;
            const std::string &a = t_[i + 1].s;
            return a == "(" || a == "::" || a == ";" || a == "," || a == ")" ||
                   a == "]" || a == ">" || a == "=" || a == "{" || a == ":";
          }
        } else if (x == ">>" && depth) {
          depth -= 2;
          if (depth <= 0)
            return i + 1 < t_.size() &&
                   (t_[i + 1].s == "(" || t_[i + 1].s == "::" ||
                    t_[i + 1].s == ";" || t_[i + 1].s == "," ||
                    t_[i + 1].s == ")");
        }
      }
    }
    return false;
  }
  std::string parse_name(bool allow_template = true) {
    std::string r;
    if (take("::"))
      r = "::";
    bool first = true;
    for (;;) {
      if (take("template")) {
        r += "template";
      }
      if (at("operator")) {
        r += parse_operator_name();
      } else if (identifier())
        r += identifier_text();
      else if (take("~")) {
        r += "~";
        r += identifier_text();
      } else
        throw std::runtime_error("name expected");
      if (allow_template && at("<") && template_candidate(r))
        r += consume_angle_text();
      if (!take("::"))
        break;
      r += "::";
      first = false;
      (void)first;
    }
    return r;
  }
  std::string parse_operator_name() {
    need("operator");
    std::string r = "operator";
    if (tok().kind == PostTokenLiteral && s().find("\"\"") == 0) {
      r += s();
      ++p_;
      if (identifier())
        r += identifier_text();
      return r;
    }
    if (at("new") || at("delete")) {
      r += s();
      ++p_;
      if (take("[")) {
        need("]");
        r += "[]";
      }
      return r;
    }
    if (take("(")) {
      need(")");
      return r += "()";
    }
    if (take("[")) {
      need("]");
      return r += "[]";
    }
    if (type_start()) {
      if (at("::") || (identifier() && s(1) == "::")) {
        r += parse_name(false);
      } else {
        r += s();
        ++p_;
      }
      while (at("*") || at("&") || at("&&") || at("const") || at("volatile")) {
        r += s();
        ++p_;
      }
      return r;
    }
    if (!eof()) {
      r += s();
      ++p_;
      if ((r == "operator<" || r == "operator>") && at("=")) {
        r += s();
        ++p_;
      }
      if (r == "operator<" && at("<")) {
        r += s();
        ++p_;
      }
      if (r == "operator>" && at(">")) {
        r += s();
        ++p_;
      }
      if (at("=")) {
        r += s();
        ++p_;
      }
    }
    if (at("<") && known_template(r))
      r += consume_angle_text();
    return r;
  }
  std::string consume_angle_text() {
    if (!take("<"))
      return std::string();
    std::size_t begin = p_ - 1;
    int depth = 1, par = 0, br = 0, brace = 0;
    while (!eof() && depth) {
      const std::string x = s();
      if (x == "(")
        ++par;
      else if (x == ")")
        --par;
      else if (x == "[")
        ++br;
      else if (x == "]")
        --br;
      else if (x == "{")
        ++brace;
      else if (x == "}")
        --brace;
      else if (par == 0 && br == 0 && brace == 0) {
        if (x == "<") {
          bool comparison =
              p_ > 1 && t_[p_ - 1].kind == PostTokenIdentifier &&
              ((t_[p_ - 2].s == "::" && !known_template(t_[p_ - 1].s)) ||
               known_value(t_[p_ - 1].s));
          if (!comparison)
            ++depth;
        } else if (x == ">")
          --depth;
        else if (x == ">>")
          depth -= 2;
      }
      ++p_;
      if (depth < 0)
        depth = 0;
    }
    if (depth > 0)
      throw std::runtime_error("unterminated template-id");
    // Reconstruct with original token spellings, expanding a logical split >>.
    return compact_range(begin, p_);
  }
  Node name_expression(const std::string &name) {
    Node result = n("id-expression " + name);
    if (p_ > 0)
      result.location = t_[p_ - 1].location;
    return result;
  }
  Node parse_id_expression() {
    const std::size_t start = p_;
    Node result = name_expression(parse_name());
    if (start < t_.size())
      result.location = t_[start].location;
    return result;
  }
  Node parse_qualified_type() {
    std::string name;
    if (at("decltype")) {
      Node d = parse_decltype();
      return d;
    }
    const std::size_t start = p_;
    name = parse_name(true);
    if (name.empty())
      throw std::runtime_error("type name expected");
    Node result = n("type-name " + name);
    if (start < t_.size())
      result.location = t_[start].location;
    return result;
  }
  std::string label_token(const std::string &x) const {
    return syntax_token_label(x);
  }
  Node parse_decltype() {
    const std::size_t start = p_;
    need("decltype");
    need("(");
    Node x = parse_expression(1);
    need(")");
    Node d = n("decltype-specifier " + compact_range(start, p_));
    if (start < t_.size())
      d.location = t_[start].location;
    d.add(x);
    return d;
  }
  Node parse_type_specifier(bool in_declspec) {
    const std::string x = s();
    if (x == "const" || x == "volatile") {
      ++p_;
      return n(std::string(in_declspec ? "decl-specifier " : "cv-qualifier ") +
               label_token(x));
    }
    if (x == "decltype") {
      Node d = parse_decltype();
      if (in_declspec) {
        d.text = "decl-specifier " + d.text.substr(d.text.find(' ') + 1);
      }
      return d;
    }
    if (x == "typename") {
      ++p_;
      std::string name = parse_name();
      return n("type-name " + name);
    }
    if (x == "class" || x == "struct" || x == "union") {
      ++p_;
      std::string nm = identifier() ? identifier_text() : std::string();
      Node f = n("class-forward-declaration" +
                 (nm.empty() ? std::string() : " " + nm));
      f.add(n("class-key " + label_token(x)));
      return f;
    }
    if (x == "enum") {
      ++p_;
      std::string nm = identifier() ? identifier_text() : std::string();
      Node f = n("enum-specifier" + (nm.empty() ? std::string() : " " + nm));
      return f;
    }
    if (identifier() || at("::")) {
      std::string name = parse_name();
      if (in_declspec && name.find("::") == std::string::npos &&
          name.find('<') == std::string::npos)
        return n("decl-specifier TT_IDENTIFIER:" + name);
      return n(std::string(in_declspec ? "decl-specifier " : "type-name ") +
               name);
    }
    if (is_kw(x)) {
      ++p_;
      return n(
          std::string(in_declspec ? "decl-specifier " : "type-specifier ") +
          label_token(x));
    }
    throw std::runtime_error("type specifier expected");
  }
  Node parse_type_specifier_seq(bool declspec) {
    Node seq = n(declspec ? "decl-specifier-seq" : "type-specifier-seq");
    unsigned count = 0;
    bool saw_type = false;
    while (!eof() && (type_start() || (force_type_names_ && identifier()) ||
                      (!declspec && (identifier() || at("::"))) ||
                      (is_kw(s()) && s() != "typename"))) {
      if ((s() == "struct" || s() == "class" || s() == "union" ||
           s() == "enum") &&
          count)
        break;
      Node spec = parse_type_specifier(declspec);
      if (spec.text.find("KW_CONST") != std::string::npos ||
          spec.text.find("KW_VOLATILE") != std::string::npos) {
      } else
        saw_type = true;
      seq.add(spec);
      ++count;
      if (count > 30)
        throw std::runtime_error("too many type specifiers");
      // A name after a type name is a declarator, unless a builtin sequence
      // continues.
      if (identifier() && saw_type)
        break;
    }
    if (!count)
      throw std::runtime_error("missing type specifier");
    return seq;
  }
  Node parse_ptr_operator() {
    if (at("*") || at("&") || at("&&")) {
      std::string x = s();
      ++p_;
      return n("ptr-operator " + label_token(x));
    }
    if (identifier() || at("::")) {
      std::size_t save = p_;
      std::string owner;
      if (take("::"))
        owner = "::";
      if (!identifier()) {
        p_ = save;
        throw std::runtime_error("member pointer owner expected");
      }
      owner += identifier_text();
      while (at("::") && s(1) != "*") {
        owner += "::";
        ++p_;
        if (!identifier())
          break;
        owner += identifier_text();
        if (at("<"))
          owner += consume_angle_text();
      }
      if (take("::") && take("*"))
        return n("ptr-operator " + owner + "::*");
      p_ = save;
    }
    throw std::runtime_error("pointer operator expected");
  }
  std::string declarator_name(const Node &d) const {
    if (d.text.find("identifier ") == 0)
      return d.text.substr(11);
    for (std::size_t i = 0; i < d.children.size(); ++i) {
      std::string x = declarator_name(d.children[i]);
      if (!x.empty())
        return x;
    }
    return std::string();
  }
  bool member_ptr_operator_ahead() const {
    if (!(identifier() || at("::")))
      return false;
    std::size_t j = p_;
    if (s(j) == "::")
      ++j;
    while (j < t_.size()) {
      if (t_[j].kind != PostTokenIdentifier)
        break;
      ++j;
      if (j < t_.size() && t_[j].s == "<") {
        int depth = 0;
        do {
          if (t_[j].s == "<")
            ++depth;
          else if (t_[j].s == ">")
            --depth;
          else if (t_[j].s == ">>")
            depth -= 2;
          ++j;
        } while (j < t_.size() && depth > 0);
      }
      if (j + 1 < t_.size() && t_[j].s == "::" && t_[j + 1].s == "*")
        return true;
      if (j < t_.size() && t_[j].s == "::") {
        ++j;
        continue;
      }
      break;
    }
    return false;
  }
  Node parse_declarator(bool abstract = false) {
    Node d = n("declarator");
    while (at("*") || at("&") || at("&&") || member_ptr_operator_ahead()) {
      d.add(parse_ptr_operator());
      while (at("const") || at("volatile")) {
        std::string cv = s();
        ++p_;
        d.add(n("cv-qualifier " + label_token(cv)));
      }
    }
    if (take("..."))
      d.add(n("parameter-pack ..."));
    bool have_direct = false;
    if (identifier() || at("::") || at("operator") || at("~")) {
      std::string nm = parse_name();
      d.add(n("identifier " + nm));
      have_direct = true;
      if (take("..."))
        d.add(n("parameter-pack ..."));
      skip_attributes();
    } else if (abstract && at("(") &&
               (s(1) == ")" || s(1) == "..." || definite_type_start(1))) {
      d.add(parse_parameter_clause());
      have_direct = true;
    } else if (take("(")) {
      Node inner = parse_declarator(abstract);
      need(")");
      Node wrap = n("nested-declarator");
      wrap.add(inner);
      d.add(wrap);
      have_direct = true;
    } else if (!abstract)
      throw std::runtime_error("declarator expected");
    while (at("[") || at("(")) {
      if (take("[")) {
        Node a = n("array-suffix");
        if (!at("]"))
          a.add(parse_expression(2));
        need("]");
        d.add(a);
      } else {
        if (s(1) != ")" && !(semantic_mode_ && s(1) == "...") && !type_start(1))
          break;
        std::size_t parameter_start = p_;
        try {
          d.add(parse_parameter_clause());
        } catch (...) {
          if (!abstract) {
            p_ = parameter_start;
            break;
          }
          throw;
        }
      }
      have_direct = true;
    }
    while (at("const") || at("volatile") || at("&") || at("&&") ||
           at("noexcept") || at("throw") || at("->") || at("override") ||
           at("final")) {
      if (at("override") || at("final")) {
        std::string v = s();
        ++p_;
        d.add(n("virt-specifier " + label_token(v)));
      } else if (at("const") || at("volatile")) {
        Node q = n("cv-qualifier " + label_token(s()));
        ++p_;
        d.add(q);
      } else if (at("&") || at("&&")) {
        Node q = n("function-qualifier " + s());
        ++p_;
        d.add(q);
      } else if (take("noexcept")) {
        std::string v = "noexcept";
        Node q = n("function-qualifier noexcept");
        if (take("(")) {
          v += "(";
          if (!at(")")) {
            std::size_t a = p_;
            Node arg = parse_expression(1);
            v += compact_range(a, p_);
            q.add(arg);
          }
          need(")");
          v += ")";
        }
        q.text = "function-qualifier " + v;
        d.add(q);
      } else if (take("throw")) {
        std::string v = "throw";
        if (take("(")) {
          std::size_t a = p_;
          int depth = 0;
          while (!eof() && !(at(")") && depth == 0)) {
            if (at("(") || at("[") || at("<"))
              ++depth;
            else if ((at(")") || at("]") || at(">")) && depth)
              --depth;
            ++p_;
          }
          v += "(" + compact_range(a, p_) + ")";
          need(")");
        }
        d.add(n("function-qualifier " + v));
      } else if (take("->")) {
        Node ti = parse_type_id();
        std::string desc = "trailing-return-type";
        if (!ti.children.empty() && !ti.children[0].children.empty()) {
          std::string type = ti.children[0].children[0].text;
          std::size_t z = type.find(' ');
          desc += " " + (z == std::string::npos ? type : type.substr(z + 1));
        }
        Node tr = n(desc);
        tr.add(ti);
        d.add(tr);
      }
    }
    if (!have_direct && !abstract)
      throw std::runtime_error("invalid declarator");
    return d;
  }
  std::string render_inline(const Node &x) const {
    std::string r = x.text;
    for (std::size_t i = 0; i < x.children.size(); ++i)
      r += render_inline(x.children[i]);
    return r;
  }
  Node parse_parameter_clause() {
    ScopeGuard parameter_scope(*this);
    return parse_parameter_clause_contents();
  }
  Node parse_parameter_clause_contents() {
    need("(");
    const std::size_t clause_begin = p_;
    Node p = n("parameter-clause");
    if (take(")"))
      return p;
    if (at("void") && s(1) == ")") {
      ++p_;
      need(")");
      return p;
    }
    for (;;) {
      if (take("...")) {
        p.add(n("parameter-pack ..."));
        need(")");
        return p;
      }
      skip_attributes();
      Node param = n("parameter-declaration");
      Node specs = parse_type_specifier_seq(true);
      param.add(specs);
      if (!at(",") && !at(")") && !at("=")) {
        Node decl = parse_declarator(true);
        if (!decl.children.empty()) {
          std::string id = declarator_name(decl);
          if (!id.empty())
            declare_value(id);
          bool abstract_function = id.empty(), has_param = false,
               has_pointer = false;
          for (std::size_t a = 0; a < decl.children.size(); ++a) {
            if (decl.children[a].text == "parameter-clause")
              has_param = true;
            if (decl.children[a].text == "nested-declarator" ||
                decl.children[a].text.find("ptr-operator") == 0)
              has_pointer = true;
          }
          bool only_pack_or_empty = false;
          for (std::size_t a = 0; a < decl.children.size(); ++a)
            if (decl.children[a].text == "parameter-clause") {
              only_pack_or_empty =
                  decl.children[a].children.empty() ||
                  (decl.children[a].children.size() == 1 &&
                   decl.children[a].children[0].text == "parameter-pack ...");
            }
          abstract_function = abstract_function && has_param && !has_pointer &&
                              only_pack_or_empty;
          if (abstract_function) {
            Node abs = n("abstract-declarator");
            abs.children.swap(decl.children);
            param.add(abs);
          } else
            param.add(decl);
        }
      }
      skip_attributes();
      if (take("=")) {
        Node da = n("default-argument");
        Node init = n("initializer");
        init.add(parse_expression(2));
        da.add(init);
        param.add(da);
      }
      p.add(param);
      if (take(")"))
        break;
      if (!take(",")) {
        std::ostringstream m;
        m << "expected comma in parameter clause begun at " << clause_begin
          << " while at " << p_ << " [" << s() << "]";
        throw std::runtime_error(m.str());
      }
    }
    return p;
  }
  Node parse_abstract_declarator() {
    Node a = n("abstract-declarator");
    bool any = false;
    while (at("*") || at("&") || at("&&") || member_ptr_operator_ahead()) {
      a.add(parse_ptr_operator());
      any = true;
    }
    if (at("(")) {
      if (semantic_mode_) {
        if (s(1) == ")" || s(1) == "..." || type_start(1)) {
          a.add(parse_parameter_clause());
          any = true;
        } else {
          ++p_;
          Node nested = parse_abstract_declarator();
          need(")");
          Node nd = n("nested-declarator"); nd.add(nested); a.add(nd); any = true;
        }
      } else if (s(1) == ")" || type_start(1) || force_type_names_) {
        a.add(parse_parameter_clause()); any = true;
      } else {
        ++p_; Node nested=parse_declarator(true); need(")");
        Node nd=n("nested-declarator"); nd.add(nested); a.add(nd); any=true;
      }
    }
    while (at("[") || at("(")) {
      if (take("[")) {
        Node ar = n("array-suffix");
        if (!at("]"))
          ar.add(parse_expression(2));
        need("]");
        a.add(ar);
      } else
        a.add(parse_parameter_clause());
      any = true;
    }
    if (!any)
      throw std::runtime_error("abstract declarator expected");
    return a;
  }
  Node parse_type_id() {
    ++force_type_names_;
    Node id = n("type-id");
    id.add(parse_type_specifier_seq(false));
    if (at("*") || at("&") || at("&&") || at("(") || at("["))
      id.add(parse_abstract_declarator());
    --force_type_names_;
    return id;
  }
  Node parse_initializer() {
    Node init = n("initializer");
    if (take("=")) {
      if (at("{"))
        init.add(parse_braced_init());
      else
        init.add(parse_expression(2));
    } else if (at("{"))
      init.add(parse_braced_init());
    else if (at("(")) {
      ++p_;
      Node pa = n("paren-initializer");
      if (!at(")")) {
        do {
          pa.add(parse_expression(2));
        } while (take(","));
      }
      need(")");
      init.add(pa);
    } else
      throw std::runtime_error("initializer expected");
    return init;
  }
  Node parse_braced_init() {
    need("{");
    Node b = n("braced-init-list");
    if (!at("}")) {
      do {
        if (take("...")) {
          if (!b.children.empty()) {
          }
        }
        b.add(parse_expression(2));
      } while (take(",") && !at("}"));
    }
    need("}");
    return b;
  }
  Node parse_decl_specs() {
    Node seq = n("decl-specifier-seq");
    bool have = false;
    bool base_seen = false;
    while (!eof()) {
      const std::string x = s();
      if (x == "typedef" || x == "extern" || x == "static" || x == "inline" ||
          x == "virtual" || x == "constexpr" || x == "thread_local" ||
          x == "friend" || x == "register" || x == "mutable") {
        seq.add(n("decl-specifier " + label_token(x)));
        ++p_;
        have = true;
        continue;
      }
      if (x == "const" || x == "volatile") {
        seq.add(n("decl-specifier " + label_token(x)));
        ++p_;
        have = true;
        continue;
      }
      if (x == "auto" || x == "bool" || x == "char" || x == "char16_t" ||
          x == "char32_t" || x == "double" || x == "float" || x == "int" ||
          x == "long" || x == "short" || x == "signed" || x == "unsigned" ||
          x == "void" || x == "wchar_t") {
        seq.add(n("decl-specifier " + label_token(x)));
        ++p_;
        have = true;
        base_seen = true;
        continue;
      }
      if (x == "decltype") {
        Node d = parse_decltype();
        d.text = "decl-specifier " + d.text.substr(d.text.find(' ') + 1);
        seq.add(d);
        have = true;
        base_seen = true;
        continue;
      }
      if (x == "class" || x == "struct" || x == "union") {
        const std::size_t class_begin=p_;
        Node c = parse_class_inline(x);
        if(semantic_mode_) {
          bool has_body=false;
          for(std::size_t q=class_begin;q<p_;++q)if(t_[q].s=="{")has_body=true;
          if(!has_body && c.text.find("class-specifier ")==0) {
            Node f=n("class-forward-declaration"+c.text.substr(std::string("class-specifier").size()));
            if(!c.children.empty())f.add(c.children[0]);
            c=f;
          }
        }
        if (have && seq.children.size() &&
            seq.children[0].text.find("KW_FRIEND") != std::string::npos &&
            !c.children.empty()) {
          std::string nm = c.text.substr(std::string("class-specifier").size());
          while (!nm.empty() && nm[0] == ' ')
            nm.erase(nm.begin());
          Node f = n("class-forward-declaration" +
                     (nm.empty() ? std::string() : " " + nm));
          f.add(c.children[0]);
          c = f;
        }
        seq.add(c);
        have = true;
        base_seen = true;
        continue;
      }
      if (x == "enum") {
        ++p_;
        Node e = n("enum-specifier");
        if (take("class") || take("struct")) {
          std::string k = t_[p_ - 1].s;
          e.add(n("enum-key " + label_token(k)));
        }
        std::string nm;
        if (identifier())
          nm = identifier_text();
        if (take(":"))
          e.add(parse_type_specifier_seq(false));
        if (take("{")) {
          if (!at("}")) {
            for (;;) {
              std::string id = identifier_text();
              Node v = n("enumerator " + id);
              if (take("="))
                v.add(parse_expression(2));
              e.add(v);
              if (!take(","))
                break;
              if (at("}"))
                break;
            }
          }
          need("}");
        }
        if (!nm.empty())
          e.text += " " + nm;
        seq.add(e);
        if (!nm.empty())
          declare_type(nm);
        have = true;
        base_seen = true;
        continue;
      }
      if (x == "typename" || type_start() || (semantic_mode_ && identifier() && s(1) == "::")) {
        if (identifier() && base_seen && !(s(1) == "::" && s(2) == "*"))
          break;
        if (identifier() &&
            ((s(1) == "::" && s(2) == "*") ||
             (s(1) != "::" && (!category_is_type(x) || base_seen))))
          break;
        if (identifier() && !known_type(x) && s(1) != "::")
          break;
        if (x == "typename")
          ++p_;
        std::string name = parse_name();
        std::string typ = "decl-specifier ";
        if (name.find("::") == std::string::npos &&
            name.find('<') == std::string::npos)
          typ += "TT_IDENTIFIER:" + name;
        else
          typ += name;
        seq.add(n(typ));
        have = true;
        base_seen = true;
        continue;
      }
      break;
    }
    if (!have)
      throw std::runtime_error("declaration specifier expected");
    return seq;
  }
  Node parse_init_declarator_list(Node *specs = 0) {
    Node list = n("init-declarator-list");
    for (;;) {
      Node item = n("init-declarator");
      Node d = parse_declarator();
      std::string name = declarator_name(d);
      item.add(d);
      if (at("=") || at("{") || at("(")) {
        // Function suffix belongs to declarator; direct-initializer is an
        // initializer only if no function suffix.
        bool function = false, nested = false;
        for (std::size_t z = 0; z < d.children.size(); ++z) {
          if (d.children[z].text == "parameter-clause")
            function = true;
          if (d.children[z].text == "nested-declarator")
            nested = true;
        }
        function = function && !nested;
        if (!(at("(") && function))
          item.add(parse_initializer());
      }
      if (specs && specs->children.size() &&
          specs->children[0].text.find("KW_TYPEDEF") != std::string::npos)
        declare_type(name);
      else
        declare_value(name);
      list.add(item);
      if (!take(","))
        break;
    }
    return list;
  }
  int precedence(const std::string &x) const {
    if (x == ",")
      return 1;
    if (x == "=" || x == "*=" || x == "/=" || x == "%=" || x == "+=" ||
        x == "-=" || x == "<<=" || x == ">>=" || x == "&=" || x == "^=" ||
        x == "|=")
      return 2;
    if (x == "?")
      return 3;
    if (x == "||")
      return 4;
    if (x == "&&")
      return 5;
    if (x == "|")
      return 6;
    if (x == "^")
      return 7;
    if (x == "&")
      return 8;
    if (x == "==" || x == "!=")
      return 9;
    if (x == "<" || x == ">" || x == "<=" || x == ">=")
      return 10;
    if (x == "<<" || x == ">>")
      return 11;
    if (x == "+" || x == "-")
      return 12;
    if (x == "*" || x == "/" || x == "%")
      return 13;
    return 0;
  }
  Node parse_expression(int minp) {
    Node left = parse_unary();
    left = parse_postfix(left);
    for (;;) {
      const std::string op = s();
      int prec = precedence(op);
      if (prec < minp || !prec)
        break;
      ++p_;
      if (op == "?") {
        Node cond = n("conditional-expression");
        cond.add(left);
        cond.add(parse_expression(1));
        need(":");
        cond.add(parse_expression(2));
        left = cond;
        continue;
      }
      const bool assign = prec == 2;
      const bool comma = op == ",";
      Node bin = n(std::string(assign  ? "assignment-expression "
                               : comma ? "binary-expression "
                                       : "binary-expression ") +
                   label_token(op));
      bin.add(left);
      bin.add(parse_expression(prec + (assign ? 0 : 1)));
      left = bin;
    }
    return left;
  }
  Node parse_unary() {
    const std::string x = s();
    if (x == "+" || x == "-" || x == "!" || x == "~" || x == "*" || x == "&" ||
        x == "++" || x == "--") {
      ++p_;
      Node u = n("unary-expression " + label_token(x));
      Node operand = parse_unary();
      operand = parse_postfix(operand);
      u.add(operand);
      return u;
    }
    if (x == "decltype" && s(1) == "(") {
      std::size_t start = p_;
      parse_decltype();
      while (take("::")) {
        if (take("template")) {
        }
        if (identifier())
          identifier_text();
        else if (at("operator"))
          parse_operator_name();
        else
          throw std::runtime_error("qualified decltype name expected");
        if (at("<") && template_candidate(s()))
          consume_angle_text();
      }
      return n("id-expression " + compact_range(start, p_));
    }
    if (x == "sizeof") {
      ++p_;
      if (take("...")) {
        need("(");
        std::string pack = identifier_text();
        need(")");
        Node z = n("sizeof-pack-expression " + pack);
        return z;
      }
      if (at("(") && definite_type_start(1) &&
          !(identifier(1) && s(2) == "(" && s(3) == ")")) {
        ++p_;
        Node z = n("sizeof-expression");
        z.add(parse_type_id());
        need(")");
        return z;
      }
      Node z = n("sizeof-expression");
      if (take("(")) {
        Node inner = parse_expression(1);
        need(")");
        if (inner.text == "parenthesized-expression" &&
            inner.children.size() == 1)
          inner = inner.children[0];
        z.add(inner);
      } else
        z.add(parse_unary());
      return z;
    }
    if (x == "alignof" || x == "typeid" || x == "noexcept") {
      ++p_;
      std::string key = x;
      std::string l = x == "alignof"  ? "KW_ALIGNOF:alignof"
                      : x == "typeid" ? "KW_TYPEID:typeid"
                                      : "KW_NOEXCEPT:noexcept";
      Node z = n("type-trait-expression " + l);
      if (take("(")) {
        if (type_start()) {
          z.add(parse_type_id());
        } else
          z.add(parse_expression(1));
        need(")");
      } else
        z.add(parse_unary());
      return z;
    }
    if (x == "static_cast" || x == "const_cast" || x == "dynamic_cast" ||
        x == "reinterpret_cast") {
      ++p_;
      Node c = n("cast-expression " + label_token(x));
      need("<");
      c.add(parse_type_id());
      close_angle();
      need("(");
      c.add(parse_expression(1));
      need(")");
      return c;
    }
    if (semantic_mode_ && type_start() && !identifier() && !at("::")) {
      std::size_t save=p_;
      try {
        Node sequence=parse_type_specifier_seq(false);
        if(at("(")) {
          Node type_id=n("type-id");type_id.add(sequence);
          need("(");
          Node cast=n("cast-expression");cast.add(type_id);
          if(!at(")"))cast.add(parse_expression(1));
          need(")");
          return cast;
        }
      } catch(const std::exception &) {}
      p_=save;
    }
    if (type_start() && s(1) == "(" && !identifier()) {
      std::string name = s();
      ++p_;
      Node base = n("id-expression " + name);
      need("(");
      Node call = n("call-expression");
      call.add(base);
      Node args = n("paren-argument-list");
      if (!at(")")) {
        do {
          args.add(parse_expression(2));
        } while (take(","));
      }
      need(")");
      call.add(args);
      return call;
    }
    if (x == "new")
      return parse_new_expression();
    if (x == "::" && s(1) == "new") {
      ++p_;
      return parse_new_expression(true);
    }
    if (x == "::" && s(1) == "delete") {
      p_ += 2;
      Node d = n("delete-expression");
      d.add(n("global-scope"));
      bool arr = take("[");
      if (arr)
        need("]");
      if (arr)
        d.add(n("array-delete"));
      d.add(parse_unary());
      return d;
    }
    if (x == "delete") {
      ++p_;
      Node d = n("delete-expression");
      bool arr = take("[");
      if (arr)
        need("]");
      if (arr)
        d.add(n("array-delete"));
      d.add(parse_unary());
      return d;
    }
    if (x == "throw") {
      ++p_;
      Node d = n("throw-expression");
      if (!at(";") && !at(")") && !at(","))
        d.add(parse_expression(1));
      return d;
    }
    if (x == "[")
      return parse_lambda();
    if (literal()) {
      const Tok value = t_[p_++];
      Node result = n("literal " + value.s);
      result.location = value.location;
      return result;
    }
    if (x == "true" || x == "false" || x == "nullptr" || x == "this") {
      ++p_;
      return n("keyword-literal " + label_token(x));
    }
    if (x == "(") {
      ++p_;
      if (definite_type_start()) {
        std::size_t save = p_;
        try {
          Node ti = parse_type_id();
          if (take(")")) {
            Node cast = n("cast-expression OP_LPAREN:");
            cast.add(ti);
            cast.add(parse_unary());
            return cast;
          }
        } catch (...) {
        }
        p_ = save;
      }
      Node inner = parse_expression(1);
      need(")");
      Node e = n("parenthesized-expression");
      e.add(inner);
      return e;
    }
    if (x == "{")
      return parse_braced_init();
    if (at("typename")) {
      ++p_;
      return name_expression(parse_name());
    }
    if (identifier() || at("::") || at("operator"))
      return parse_id_expression();
    throw std::runtime_error("expression expected at " + x);
  }
  void close_angle() {
    if (take(">"))
      return;
    if (at(">>")) {
      ++p_;
      return;
    }
    throw std::runtime_error("template close expected");
  }
  Node parse_postfix(Node base) {
    for (;;) {
      if (take("(")) {
        Node call = n("call-expression");
        call.add(base);
        Node args = n("argument-list");
        if (!at(")")) {
          do {
            if (take("...")) {
              args.add(n("pack-expansion-expression"));
              continue;
            }
            args.add(parse_expression(2));
          } while (take(","));
        }
        need(")");
        call.add(args);
        base = call;
      } else if (at("{")) {
        Node call = n("call-expression");
        call.add(base);
        call.add(parse_braced_init());
        base = call;
      } else if (take("[")) {
        Node x = n("subscript-expression");
        x.add(base);
        x.add(parse_expression(1));
        need("]");
        base = x;
      } else if (at(".") || at("->")) {
        std::string op = s();
        ++p_;
        Node m = n("member-expression " + label_token(op));
        m.add(base);
        bool dependent_template = take("template");
        std::string member;
        if (at("operator"))
          member = parse_operator_name();
        else if (at("~"))
          member = parse_name();
        else if (dependent_template)
          member = "template " + parse_name();
        else if (s(1) == "::")
          member = parse_name();
        else
          member = identifier_text();
        if (dependent_template && member.find('<') == std::string::npos &&
            at("<"))
          member += consume_angle_text();
        m.add(n("identifier " + member));
        base = m;
      } else if (take("...")) {
        Node x = n("pack-expansion-expression");
        x.add(base);
        base = x;
      } else if (at("++") || at("--")) {
        std::string op = s();
        ++p_;
        Node x = n("postfix-expression " + label_token(op));
        x.add(base);
        base = x;
      } else if (at(".*") || at("->*")) {
        std::string op = s();
        ++p_;
        Node m = n("binary-expression " + label_token(op));
        m.add(base);
        m.add(parse_unary());
        base = m;
      } else
        break;
    }
    return base;
  }
  Node parse_new_expression(bool global = false) {
    need("new");
    Node e = n("new-expression");
    if (global || take("::"))
      e.add(n("global-scope"));
    bool parenthesized_type = false;
    if (at("(") && type_start(1)) {
      ++p_;
      parenthesized_type = true;
      Node ti = n("type-id");
      ti.add(parse_type_specifier_seq(false));
      if (at("*") || at("&") || at("&&") || at("["))
        ti.add(parse_abstract_declarator());
      need(")");
      e.add(ti);
    } else if (take("(")) {
      std::size_t a = p_;
      Node args = n("paren-argument-list");
      if (!at(")")) {
        do {
          args.add(parse_expression(2));
        } while (take(","));
      }
      std::string content = compact_range(a, p_);
      need(")");
      Node pl = n("placement (" + content + ")");
      pl.add(args);
      e.add(pl);
    }
    if (!parenthesized_type) {
      Node ti = n("type-id");
      ti.add(parse_type_specifier_seq(false));
      if (at("*") || at("&") || at("&&") || at("["))
        ti.add(parse_abstract_declarator());
      e.add(ti);
    }
    if (at("(") || at("{")) {
      Node init = n("initializer");
      if (take("(")) {
        Node a = n("paren-initializer");
        if (!at(")")) {
          do {
            a.add(parse_expression(2));
          } while (take(","));
        }
        need(")");
        init.add(a);
      } else
        init.add(parse_braced_init());
      e.add(init);
    }
    return e;
  }
  Node parse_lambda() {
    need("[");
    std::string capture = "[";
    bool first = true;
    while (!eof() && !at("]")) {
      if (!first)
        capture += ",";
      first = false;
      if (take("&")) {
        capture += "&";
        if (identifier())
          capture += identifier_text();
      } else if (take("="))
        capture += "=";
      else if (take("this"))
        capture += "this";
      else if (identifier()) {
        capture += identifier_text();
        if (take("..."))
          capture += "...";
      } else if (take("..."))
        capture += "...";
      else
        throw std::runtime_error("invalid capture");
      if (!take(","))
        break;
    }
    need("]");
    capture += "]";
    Node l = n("lambda-expression");
    l.add(n("lambda-introducer " + capture));
    Node lambda_declarator;
    bool has_lambda_declarator = false;
    if (at("(") || at("mutable") || at("noexcept") || at("->")) {
      lambda_declarator = n("lambda-declarator");
      has_lambda_declarator = true;
      if (at("("))
        lambda_declarator.add(parse_parameter_clause());
      if (take("mutable"))
        lambda_declarator.add(n("lambda-specifier KW_MUTABLE:mutable"));
      if (take("noexcept")) {
        Node f = n("noexcept-specification");
        if (take("(")) {
          if (!at(")"))
            f.add(parse_expression(1));
          need(")");
        }
        lambda_declarator.add(f);
      }
      if (take("->")) {
        Node tr = n("trailing-return-type");
        tr.add(parse_type_id());
        lambda_declarator.add(tr);
      }
      l.add(lambda_declarator);
    }
    if (has_lambda_declarator)
      l.add(parse_function_compound(lambda_declarator));
    else
      l.add(parse_compound());
    return l;
  }
  void skip_attributes() {
    for (;;) {
      if (at("[") && s(1) == "[") {
        p_ += 2;
        int depth = 1;
        while (!eof() && depth) {
          if (at("[") && s(1) == "[") {
            depth++;
            p_ += 2;
          } else if (at("]") && s(1) == "]") {
            depth--;
            p_ += 2;
          } else
            ++p_;
        }
        if (depth)
          throw std::runtime_error("unterminated attribute");
        continue;
      }
      if (at("__attribute__")) {
        ++p_;
        if (take("(")) {
          int depth = 1;
          while (!eof() && depth) {
            if (take("("))
              ++depth;
            else if (take(")"))
              --depth;
            else
              ++p_;
          }
          if (depth)
            throw std::runtime_error("unterminated attribute");
        }
        continue;
      }
      if (at("alignas")) {
        ++p_;
        if (take("(")) {
          int depth = 1;
          while (!eof() && depth) {
            if (take("("))
              ++depth;
            else if (take(")"))
              --depth;
            else
              ++p_;
          }
          if (depth)
            throw std::runtime_error("unterminated alignas");
        }
        continue;
      }
      break;
    }
  }
  Node parse_template_parameter_clause() {
    need("template");
    need("<");
    Node clause = n("template-parameter-clause");
    if (take(">"))
      return clause;
    Node list = n("template-parameter-list");
    for (;;) {
      if (take("template")) {
        Node param = n("type-parameter");
        Node tp = n("template-template-parameter");
        param.add(tp);
        // parse nested parameter clause's opening <, without leading template
        // keyword
        need("<");
        Node nested = n("template-parameter-clause");
        Node nl = n("template-parameter-list");
        if (!at(">")) {
          do {
            Node sub = n("type-parameter");
            if (at("class") || at("typename")) {
              std::string q = s();
              ++p_;
              sub.add(n("parameter-key " + label_token(q)));
              if (take("..."))
                sub.add(n("parameter-pack ..."));
              if (identifier()) {
                std::string nm = identifier_text();
                sub.add(n("identifier " + nm));
                types_.back().insert(nm);
              }
            } else
              throw std::runtime_error("nested template parameter");
            nl.add(sub);
          } while (take(","));
        }
        close_angle();
        nested.add(nl);
        param.add(nested);
        need("class");
        param.add(n("parameter-key KW_CLASS:class"));
        if (identifier()) {
          std::string nm = identifier_text();
          param.add(n("identifier " + nm));
          values_.back().erase(nm);
          templates_.back().insert(nm);
          types_.back().erase(nm);
        }
        if (take("=")) {
          Node da = n("default-template-argument");
          da.add(parse_type_id());
          param.add(da);
        }
        list.add(param);
      } else if (at("class") ||
                 (at("typename") &&
                  (!identifier(1) || s(2) == "," || s(2) == ">" ||
                   s(2) == ">>" || s(2) == "=" || s(2) == "..."))) {
        std::string key = s();
        ++p_;
        Node param = n("type-parameter");
        param.add(n("parameter-key " + label_token(key)));
        if (take("..."))
          param.add(n("parameter-pack ..."));
        std::string nm;
        if (identifier()) {
          nm = identifier_text();
          param.add(n("identifier " + nm));
          values_.back().erase(nm);
          types_.back().insert(nm);
          templates_.back().erase(nm);
        }
        if (take("=")) {
          Node da = n("default-template-argument");
          da.add(parse_type_id());
          param.add(da);
        }
        list.add(param);
      } else {
        Node param = n("non-type-template-parameter");
        Node specs = parse_decl_specs();
        param.add(specs);
        if (take("..."))
          param.add(n("parameter-pack ..."));
        std::string nm;
        if (!at(",") && !at(">") && !at(">>") && !at("=")) {
          Node d = parse_declarator();
          nm = declarator_name(d);
          param.add(d);
        }
        if (!nm.empty()) {
          values_.back().insert(nm);
          types_.back().erase(nm);
          templates_.back().erase(nm);
        }
        types_.back().erase(nm);
        templates_.back().erase(nm);
        if (take("=")) {
          Node da = n("default-template-argument");
          if (literal()) {
            bool lone_int =
                nm.empty() && specs.children.size() == 1 &&
                specs.children[0].text == "decl-specifier KW_INT:int";
            da.add(
                n(lone_int ? "literal TT_LITERAL:" + s() : "literal " + s()));
            ++p_;
          } else
            da.add(parse_expression(11));
          param.add(da);
        }
        list.add(param);
      }
      if (!take(","))
        break;
    }
    close_angle();
    clause.add(list);
    return clause;
  }
  Node parse_class_name() {
    if (identifier() || at("::")) {
      std::string nm = parse_name();
      return n("class-name " + nm);
    }
    return n("class-name");
  }
  Node parse_base_clause() {
    if (!take(":"))
      return Node();
    Node c = n("base-clause");
    for (;;) {
      Node b = n("base-specifier");
      if (take("virtual"))
        b.add(n("virtual KW_VIRTUAL:virtual"));
      if (at("public") || at("private") || at("protected")) {
        std::string a = s();
        ++p_;
        b.add(n("access-specifier " + label_token(a)));
      }
      if (take("virtual"))
        b.add(n("virtual KW_VIRTUAL:virtual"));
      std::string base;
      if (at("decltype")) {
        std::size_t start = p_;
        parse_decltype();
        base = compact_range(start, p_);
        b.add(n("base-name " + base));
      } else {
        if (take("typename"))
          base = "typename";
        base += parse_name();
        b.add(n("base-name " + base));
      }
      if (take("..."))
        b.add(n("pack-expansion OP_DOTS:..."));
      c.add(b);
      if (!take(","))
        break;
    }
    return c;
  }
  void preserve_qualified_class_facts() {
    const std::vector<std::uint32_t> &type_slots = types_.back().slots();
    for (std::size_t i = 0; i < type_slots.size(); ++i)
      if (type_slots[i]) qualified_types_.insert_id(type_slots[i] - 1);
    const std::vector<std::uint32_t> &template_slots = templates_.back().slots();
    for (std::size_t i = 0; i < template_slots.size(); ++i)
      if (template_slots[i]) qualified_templates_.insert_id(template_slots[i] - 1);
  }
  void expose_qualified_class_facts(const Node &declarator) {
    const bool qualified = declarator_name(declarator).find("::") !=
                           std::string::npos;
    if (class_names_.empty() && !qualified) return;
    const std::vector<std::uint32_t> &type_slots = qualified_types_.slots();
    for (std::size_t i = 0; i < type_slots.size(); ++i)
      if (type_slots[i]) types_.back().insert_id(type_slots[i] - 1);
    const std::vector<std::uint32_t> &template_slots =
        qualified_templates_.slots();
    for (std::size_t i = 0; i < template_slots.size(); ++i)
      if (template_slots[i]) templates_.back().insert_id(template_slots[i] - 1);
  }
  Node parse_class() {
    std::string key = s();
    if (key != "class" && key != "struct" && key != "union")
      throw std::runtime_error("class key expected");
    ++p_;
    std::string name;
    if (identifier() || at("::"))
      name = parse_name();
    if (!at("{") && !at(":")) {
      need(";");
      declare_type(name);
      Node f = n("class-forward-declaration" +
                 (name.empty() ? std::string() : " " + name));
      f.add(n("class-key " + label_token(key)));
      return f;
    }
    Node bases = parse_base_clause();
    need("{");
    if (!name.empty())
      declare_type(name);
    class_names_.push_back(name);
    push_scope();
    Node body =
        n("class-specifier" + (name.empty() ? std::string() : " " + name));
    body.add(n("class-key " + label_token(key)));
    if (!bases.text.empty())
      body.add(bases);
    while (!eof() && !at("}")) {
      skip_attributes();
      if (at("}"))
        break;
      if ((at("public") || at("protected") || at("private")) && s(1) == ":") {
        std::string a = s();
        p_ += 2;
        body.add(n("access-specifier " + label_token(a)));
        continue;
      }
      if (take(";")) {
        body.add(n("empty-declaration"));
        continue;
      }
      const std::size_t before = p_;
      Node m = parse_declaration(true);
      body.add(m);
      if (p_ == before)
        throw std::runtime_error("class member made no progress");
    }
    need("}");
    preserve_qualified_class_facts();
    pop_scope();
    class_names_.pop_back();
    take(";");
    return body;
  }
  Node parse_enum() {
    need("enum");
    Node e = n("enum-specifier");
    if (take("class") || take("struct")) {
      std::string x = t_[p_ - 1].s;
      e.add(n("enum-key " + label_token(x)));
    }
    std::string name;
    if (identifier() || (semantic_mode_ && at("::")))
      name = semantic_mode_ ? parse_name() : identifier_text();
    if (!name.empty())
      declare_type(name);
    if (take(":")) { // underlying enum type is retained as type syntax
      Node u = parse_type_specifier_seq(false);
      e.add(u);
    }
    if (take("{")) {
      if (!at("}")) {
        for (;;) {
          std::string id = identifier_text();
          Node v = n("enumerator " + id);
          values_.back().insert(id);
          if (take("="))
            v.add(parse_expression(2));
          e.add(v);
          if (!take(","))
            break;
          if (at("}"))
            break;
        }
      }
      need("}");
    }
    if (!name.empty())
      e.text += " " + name;
    return e;
  }
  Node parse_using() {
    need("using");
    if (take("namespace")) {
      Node u = n("using-directive");
      u.add(n("target " + parse_name()));
      need(";");
      return u;
    }
    if (identifier() && s(1) == "=") {
      std::string name = identifier_text();
      need("=");
      Node a = n("alias-declaration " + name);
      a.add(parse_type_id());
      need(";");
      declare_type(name);
      return a;
    }
    std::string target = parse_name();
    Node u = n("using-declaration");
    u.add(n("target " + target));
    need(";");
    std::size_t q = target.rfind("::");
    std::string tail = q == std::string::npos ? target : target.substr(q + 2);
    declare_type(tail);
    templates_.back().insert(tail);
    return u;
  }
  Node parse_namespace() {
    need("namespace");
    std::string name;
    if (identifier())
      name = identifier_text();
    if (take("=")) {
      Node a = n("namespace-alias-definition " + name);
      a.add(n("target " + parse_name()));
      need(";");
      return a;
    }
    need("{");
    Node ns = n("namespace-definition " +
                (name.empty() ? std::string("<unnamed>") : name));
    push_scope();
    while (!eof() && !at("}"))
      ns.add(parse_declaration());
    need("}");
    const std::vector<std::uint32_t> &type_slots = types_.back().slots();
    for (std::size_t i = 0; i < type_slots.size(); ++i)
      if (type_slots[i]) types_[types_.size() - 2].insert_id(type_slots[i] - 1);
    const std::vector<std::uint32_t> &template_slots = templates_.back().slots();
    for (std::size_t i = 0; i < template_slots.size(); ++i)
      if (template_slots[i]) templates_[templates_.size() - 2].insert_id(template_slots[i] - 1);
    const std::vector<std::uint32_t> &value_slots = values_.back().slots();
    for (std::size_t i = 0; i < value_slots.size(); ++i)
      if (value_slots[i]) values_[values_.size() - 2].insert_id(value_slots[i] - 1);
    pop_scope();
    return ns;
  }
  Node parse_static_assert() {
    need("static_assert");
    need("(");
    Node a = n("static-assert-declaration");
    a.add(parse_expression(2));
    if (take(",")) {
      if (!literal())
        throw std::runtime_error("static assert message must be string");
      a.add(n("message " + s()));
      ++p_;
    }
    need(")");
    need(";");
    return a;
  }
  Node parse_linkage() {
    need("extern");
    std::string lang;
    if (!literal())
      throw std::runtime_error("linkage string expected");
    lang = s().substr(1, s().size() - 2);
    ++p_;
    Node l = n("linkage-specification " + lang);
    if (take("{")) {
      while (!eof() && !at("}"))
        l.add(parse_declaration());
      need("}");
    } else
      l.add(parse_declaration());
    return l;
  }
  Node parse_condition() {
    Node c = n("condition");
    if (definite_type_start()) {
      std::size_t save = p_;
      try {
        Node specs = parse_decl_specs();
        Node d = parse_declarator();
        if (take("=")) {
          Node cd = n("condition-declaration");
          cd.add(specs);
          cd.add(d);
          Node init = n("initializer");
          init.add(parse_expression(2));
          cd.add(init);
          c.add(cd);
          return c;
        }
      } catch (...) {
      }
      p_ = save;
    }
    c.add(parse_expression(1));
    return c;
  }
  Node parse_compound() {
    need("{");
    Node b = n("compound-statement");
    push_scope();
    while (!eof() && !at("}")) {
      skip_attributes();
      if (at("}"))
        break;
      std::size_t before = p_;
      b.add(parse_statement());
      if (p_ == before)
        throw std::runtime_error("statement made no progress");
    }
    need("}");
    pop_scope();
    return b;
  }
  Node parse_statement() {
    if (at("{"))
      return parse_compound();
    if (take(";"))
      return n("expression-statement");
    if (take("if")) {
      Node i = n("if-statement");
      need("(");
      i.add(parse_condition());
      need(")");
      Node th = n("then");
      th.add(parse_statement());
      i.add(th);
      if (take("else")) {
        Node el = n("else");
        el.add(parse_statement());
        i.add(el);
      }
      return i;
    }
    if (take("switch")) {
      Node x = n("switch-statement");
      need("(");
      x.add(parse_condition());
      need(")");
      x.add(parse_statement());
      return x;
    }
    if (take("while")) {
      Node x = n("while-statement");
      need("(");
      x.add(parse_condition());
      need(")");
      x.add(parse_statement());
      return x;
    }
    if (take("do")) {
      Node x = n("do-statement");
      x.add(parse_statement());
      need("while");
      need("(");
      Node c = n("condition");
      c.add(parse_expression(1));
      x.add(c);
      need(")");
      need(";");
      return x;
    }
    if (take("for")) {
      need("(");
      Node x;
      if (take(";")) {
        x = n("for-statement");
      } else {
        std::size_t save = p_;
        if (type_start() || at("auto") || at("typedef")) {
          try {
            Node specs = parse_decl_specs();
            Node d = parse_declarator();
            if (take(":")) {
              x = n("range-for-statement");
              Node rd = n("range-declaration");
              rd.add(specs);
              rd.add(d);
              x.add(rd);
              Node ri = n("range-initializer");
              ri.add(parse_expression(1));
              x.add(ri);
              need(")");
              x.add(parse_statement());
              return x;
            }
          } catch (...) {
          }
          p_ = save;
        }
        x = n("for-statement");
        Node fi = n("for-init-statement");
        if (type_start() || at("auto") || at("typedef"))
          fi.add(parse_declaration(false));
        else {
          fi.add(parse_expression(1));
          need(";");
        }
        x.add(fi);
      }
      if (!at(";"))
        x.add(parse_condition());
      need(";");
      if (!at(")")) {
        Node it = n("iteration");
        it.add(parse_expression(1));
        x.add(it);
      }
      need(")");
      x.add(parse_statement());
      return x;
    }
    if (take("return")) {
      Node r = n("return-statement");
      if (!at(";"))
        r.add(parse_expression(1));
      need(";");
      return r;
    }
    if (take("break")) {
      need(";");
      return n("break-statement");
    }
    if (take("continue")) {
      need(";");
      return n("continue-statement");
    }
    if (take("goto")) {
      std::string id = identifier_text();
      need(";");
      return n("goto-statement " + id);
    }
    if (take("case")) {
      Node c = n("case-statement");
      c.add(parse_expression(2));
      need(":");
      c.add(parse_statement());
      return c;
    }
    if (take("default")) {
      Node d = n("default-statement");
      need(":");
      d.add(parse_statement());
      return d;
    }
    if (identifier() && s(1) == ":") {
      std::string id = identifier_text();
      need(":");
      Node l = n("labeled-statement " + id);
      l.add(parse_statement());
      return l;
    }
    if (take("try")) {
      Node tr = n("try-block");
      tr.add(parse_compound());
      while (take("catch")) {
        Node h = n("handler");
        need("(");
        if (take("...")) {
          Node ex = n("exception-declaration");
          ex.add(n("ellipsis ..."));
          h.add(ex);
        } else {
          Node ex = n("exception-declaration");
          if (!at(")")) {
            ex.add(parse_decl_specs());
            if (!at(")"))
              ex.add(parse_declarator(true));
          }
          h.add(ex);
        }
        need(")");
        h.add(parse_compound());
        tr.add(h);
      }
      return tr;
    }
    if (take("throw")) {
      Node x = n("throw-statement");
      if (!at(";"))
        x.add(parse_expression(1));
      need(";");
      return x;
    }
    // A declaration is selected for known/fallback type names per the PA5
    // declaration-preference rule; known values and parameters override hints.
    bool expr_disambiguates = false;
    if (type_start() && identifier() && s(1) == "(") {
      int d = 0;
      for (std::size_t k = p_ + 1; k < t_.size(); ++k) {
        if (t_[k].s == "(")
          ++d;
        else if (t_[k].s == ")" && --d == 0) {
          if (k + 1 < t_.size() && (t_[k + 1].s == "->" ||
                                    t_[k + 1].s == "++" || t_[k + 1].s == "--"))
            expr_disambiguates = true;
          break;
        }
      }
    }
    if (!expr_disambiguates &&
        (at("typedef") || at("using") || at("static_assert") ||
         at("template") || at("namespace") || at("class") || at("struct") ||
         at("union") || at("enum") || at("extern") || type_start())) {
      std::size_t save = p_;
      try {
        return parse_declaration(false);
      } catch (...) {
        p_ = save;
      }
    }
    Node e = n("expression-statement");
    e.add(parse_expression(1));
    need(";");
    return e;
  }
  Node parse_class_inline(const std::string &key) {
    ++p_;
    if (at("alignas")) {
      ++p_;
      if (take("(")) {
        if (type_start())
          parse_type_id();
        else
          parse_expression(1);
        need(")");
      }
    }
    std::string name;
    if (identifier())
      name = parse_name();
    Node cls =
        n("class-specifier" + (name.empty() ? std::string() : " " + name));
    cls.add(n("class-key " + label_token(key)));
    if (at(":")) {
      Node b = parse_base_clause();
      cls.add(b);
    }
    if (take("{")) {
      if (!name.empty()) {
        declare_type(name);
        if (active_template_declarations_)
          templates_.back().insert(name);
      }
      class_names_.push_back(name);
      push_scope();
      if(semantic_mode_) {
        for(std::size_t k=p_;k<t_.size();++k) {
          if(t_[k].s=="}")break;
          if(t_[k].s=="typedef") {
            for(std::size_t z=k+1;z<t_.size()&&t_[z].s!=";";++z) {
              if(t_[z].s=="{" && z<brace_mate_.size() && brace_mate_[z]<t_.size()) { z=brace_mate_[z]; continue; }
              if(t_[z].kind==PostTokenIdentifier)types_.back().insert(t_[z].s);
            }
          }
          if(t_[k].s=="using" && k+2<t_.size() && t_[k+2].s=="=")
            types_.back().insert(t_[k+1].s);
          if((t_[k].s=="class" || t_[k].s=="struct" || t_[k].s=="union" || t_[k].s=="enum") &&
             k+1<t_.size() && t_[k+1].kind==PostTokenIdentifier)
            types_.back().insert(t_[k+1].s);
          if(t_[k].s=="{" && k<brace_mate_.size() && brace_mate_[k]<t_.size()) {
            k=brace_mate_[k];
          }
        }
      } else {
        int depth=0;
        for(std::size_t k=p_;k<t_.size();++k) {
          if(t_[k].s=="{")++depth;
          else if(t_[k].s=="}") { if(depth==0)break; --depth; }
          else if((t_[k].s=="class" || t_[k].s=="struct" || t_[k].s=="union") &&
                  k+1<t_.size() && t_[k+1].kind==PostTokenIdentifier)
            types_.back().insert(t_[k+1].s);
        }
      }
      while (!eof() && !at("}")) {
        skip_attributes();
        if (at("}"))
          break;
        if ((at("public") || at("private") || at("protected")) && s(1) == ":") {
          std::string a = s();
          p_ += 2;
          cls.add(n("access-specifier " + label_token(a)));
          continue;
        }
        std::size_t before = p_;
        cls.add(parse_declaration(true));
        if (before == p_)
          throw std::runtime_error("class member stalled");
      }
      need("}");
    preserve_qualified_class_facts();
      pop_scope();
      class_names_.pop_back();
    } else if (!name.empty()) {
      declare_type(name);
      if (active_template_declarations_)
        templates_.back().insert(name);
    }
    return cls;
  }
  std::string first_decl_name(const Node &x) const {
    if (x.text.find("class-specifier ") == 0 ||
        x.text.find("class-forward-declaration ") == 0) {
      std::size_t at = x.text.find(' ');
      std::string v =
          at == std::string::npos ? std::string() : x.text.substr(at + 1);
      std::size_t a = v.find('<');
      if (a != std::string::npos)
        v = v.substr(0, a);
      return v;
    }
    if (x.text.find("alias-declaration ") == 0)
      return x.text.substr(std::string("alias-declaration ").size());
    if (x.text.find("identifier ") == 0) {
      std::string v = x.text.substr(11);
      std::size_t a = v.find('<');
      if (a != std::string::npos)
        v = v.substr(0, a);
      std::size_t q = v.rfind("::");
      if (q != std::string::npos)
        v = v.substr(q + 2);
      return v;
    }
    for (std::size_t i = 0; i < x.children.size(); ++i) {
      std::string v = first_decl_name(x.children[i]);
      if (!v.empty())
        return v;
    }
    return std::string();
  }
  bool qualified_special_member_ahead() {
    std::size_t start = p_;
    for (;;) {
      if (start < t_.size() &&
          (t_[start].s == "inline" || t_[start].s == "constexpr" ||
           t_[start].s == "virtual" || t_[start].s == "explicit")) {
        ++start;
        continue;
      }
      if (start < t_.size() && t_[start].s == "__attribute__") {
        ++start;
        int dep = 0;
        if (start < t_.size() && t_[start].s == "(") {
          do {
            if (t_[start].s == "(")
              ++dep;
            else if (t_[start].s == ")")
              --dep;
            ++start;
          } while (start < t_.size() && dep);
        }
        continue;
      }
      break;
    }
    if (start >= t_.size() ||
        !(t_[start].kind == PostTokenIdentifier || t_[start].s == "::"))
      return false;
    std::size_t end = start;
    int angle = 0;
    std::vector<std::size_t> separators;
    for (; end < t_.size(); ++end) {
      const std::string &x = t_[end].s;
      if (x == "::" && angle == 0)
        separators.push_back(end);
      if (x == "<")
        ++angle;
      else if (x == ">")
        --angle;
      else if (x == ">>" && angle)
        angle -= 2;
      if (angle < 0)
        angle = 0;
      if (angle == 0 && x == "(")
        break;
      if (angle == 0 && (x == ";" || x == "{"))
        return false;
    }
    if (end == t_.size() || separators.empty())
      return false;
    std::size_t sep = separators.back();
    std::size_t owner_end = sep;
    std::size_t owner_start = start;
    for (std::size_t i = start; i < sep; ++i)
      if (t_[i].s == "::")
        owner_start = i + 1;
    std::string owner;
    for (std::size_t i = owner_start; i < owner_end; ++i)
      owner += t_[i].s;
    std::size_t a = owner.find('<');
    if (a != std::string::npos)
      owner = owner.substr(0, a);
    std::string leaf;
    for (std::size_t i = sep + 1; i < end; ++i)
      leaf += t_[i].s;
    if (owner.empty() || leaf.empty())
      return false;
    bool conversion = false;
    if (leaf.find("operator") == 0) {
      std::string tail = leaf.substr(8);
      if (tail != "new" && tail != "delete" && tail != "()" && tail != "[]" &&
          !tail.empty() &&
          (tail[0] == ':' ||
           std::isalpha(static_cast<unsigned char>(tail[0])) || tail[0] == '_'))
        conversion = true;
    }
    return leaf == owner || leaf == "~" + owner || conversion;
  }
  Node parse_declaration(bool member = false) {
    skip_attributes();
    if (take(";"))
      return n("empty-declaration");
    if (at("namespace"))
      return parse_namespace();
    if (at("inline") && s(1) == "namespace") {
      ++p_;
      Node ns = parse_namespace();
      ns.children.insert(ns.children.begin(), n("inline"));
      return ns;
    }
    if (at("using"))
      return parse_using();
    if (at("static_assert"))
      return parse_static_assert();
    if (at("template")) {
      push_scope();
      ++active_template_declarations_;
      Node td = n("template-declaration");
      td.add(parse_template_parameter_clause());
      Node d = parse_declaration(member);
      td.add(d);
      std::string declared = first_decl_name(d);
      --active_template_declarations_;
      pop_scope();
      if (!declared.empty()) {
        templates_.back().insert(declared);
        const bool is_type = d.text.find("class-specifier") == 0 ||
                             d.text.find("class-forward-declaration") == 0 ||
                             d.text.find("alias-declaration") == 0;
        if (is_type)
          types_.back().insert(declared);
        else
          values_.back().insert(declared);
      }
      return td;
    }
    if (at("extern") && literal(1))
      return parse_linkage();
    if (!member && qualified_special_member_ahead()) {
      std::vector<std::string> prefix;
      while (at("inline") || at("constexpr") || at("virtual") ||
             at("explicit")) {
        prefix.push_back(s());
        ++p_;
      }
      skip_attributes();
      Node sp = parse_special_member(true);
      if (!prefix.empty()) {
        Node ms = n("member-specifiers");
        for (std::size_t i = 0; i < prefix.size(); ++i)
          ms.add(n("specifier " + (prefix[i] == "explicit"
                                       ? prefix[i]
                                       : label_token(prefix[i]))));
        sp.children.insert(sp.children.begin(), ms);
      }
      return sp;
    }
    if (member) {
      std::vector<std::string> prefixes;
      while (at("inline") || at("virtual") || at("explicit") ||
             at("constexpr") || at("friend") || at("static")) {
        prefixes.push_back(s());
        ++p_;
      }
      if (starts_special_member(true)) {
        Node sp = parse_special_member(at("{"));
        if (!prefixes.empty()) {
          Node specs = n("member-specifiers");
          for (std::size_t i = 0; i < prefixes.size(); ++i)
            specs.add(n("specifier " + (prefixes[i] == "explicit"
                                            ? prefixes[i]
                                            : label_token(prefixes[i]))));
          sp.children.insert(sp.children.begin(), specs);
        }
        return sp;
      }
      for (std::size_t i = 0; i < prefixes.size(); ++i)
        --p_;
    }
    if (starts_special_member(member)) {
      // A constructor/destructor/conversion function has no decl-specifier
      // sequence.
      std::size_t q = p_;
      if (at("operator") && s(1) != "::" && !type_start(1)) {
      } else
        return parse_special_member(at("~") || (!class_names_.empty() &&
                                                s() == class_names_.back() &&
                                                s(1) == "("));
      p_ = q;
    }
    if (at("extern") && s(1) == "template") {
      ++p_;
      ++p_;
      Node e = n("explicit-instantiation-declaration");
      e.add(parse_declaration(member));
      return e;
    }
    if (at("class") || at("struct") || at("union")) {
      std::string key = s();
      std::size_t save = p_;
      Node cls = parse_class_inline(key);
      if (at(";")) {
        ++p_;
        if (cls.text == "class-specifier" ||
            cls.text.find("class-specifier ") == 0) {
          if (!cls.children.empty() && p_ > save + 1 && t_[save + 1].s != "{") {
          } // definition vs declaration below
        }
        // A class name without a body is a forward declaration.
        bool body = false;
        for (std::size_t i = save; i < p_; ++i)
          if (t_[i].s == "{")
            body = true;
        if (!body) {
          std::string nm =
              cls.text.substr(std::string("class-specifier").size());
          while (!nm.empty() && nm[0] == ' ')
            nm.erase(nm.begin());
          Node f = n("class-forward-declaration" +
                     (nm.empty() ? std::string() : " " + nm));
          if (!cls.children.empty())
            f.add(cls.children[0]);
          return f;
        }
        return cls;
      }
      if(semantic_mode_) {
        bool class_body=false;
        for(std::size_t q=save;q<p_;++q)if(t_[q].s=="{")class_body=true;
        if(!class_body && cls.text.find("class-specifier ")==0) {
          Node f=n("class-forward-declaration"+cls.text.substr(std::string("class-specifier").size()));
          if(!cls.children.empty())f.add(cls.children[0]);
          cls=f;
        }
      }
      Node specs = n("decl-specifier-seq");
      specs.add(cls);
      Node simple = n("simple-declaration");
      simple.add(specs);
      if (!at(";"))
        simple.add(parse_init_declarator_list(&specs));
      need(";");
      return simple;
    }
    if (at("enum")) {
      ++p_;
      Node e = n("enum-specifier");
      if (take("class") || take("struct")) {
        std::string k = t_[p_ - 1].s;
        e.add(n("enum-key " + label_token(k)));
      }
      std::string nm;
      if (identifier() || (semantic_mode_ && at("::")))
        nm = semantic_mode_ ? parse_name() : identifier_text();
      if (take(":")) {
        Node u = n("type-id");
        u.add(parse_type_specifier_seq(false));
        e.add(u);
      }
      bool body = take("{");
      if (body) {
        if (!at("}")) {
          for (;;) {
            std::string id = identifier_text();
            Node v = n("enumerator " + id);
            if (take("="))
              v.add(parse_expression(2));
            e.add(v);
            if (!take(","))
              break;
            if (at("}"))
              break;
          }
        }
        need("}");
      }
      if (!nm.empty())
        e.text += " " + nm;
      if (!nm.empty())
        declare_type(nm);
      if (take(";")) {
        if (!body && !nm.empty())
          return e;
        if (!body)
          return e;
        return e;
      }
      Node specs = n("decl-specifier-seq");
      specs.add(e);
      Node simple = n("simple-declaration");
      simple.add(specs);
      simple.add(parse_init_declarator_list(&specs));
      need(";");
      return simple;
    }
    if (at(";")) {
      ++p_;
      return n("empty-declaration");
    }
    Node specs = parse_decl_specs();
    return parse_declaration_tail(specs, member);
  }
  void bind_function_parameters(const Node &declarator) {
    for (std::size_t i = 0; i < declarator.children.size(); ++i) {
      const Node &clause = declarator.children[i];
      if (clause.text != "parameter-clause")
        continue;
      for (std::size_t j = 0; j < clause.children.size(); ++j) {
        const Node &parameter = clause.children[j];
        if (parameter.text != "parameter-declaration")
          continue;
        for (std::size_t k = 0; k < parameter.children.size(); ++k) {
          const Node &child = parameter.children[k];
          if (child.text == "declarator" ||
              child.text == "abstract-declarator") {
            const std::string name = declarator_name(child);
            if (!name.empty())
              declare_value(name);
          }
        }
      }
    }
  }
  Node parse_function_compound(const Node &declarator) {
    ScopeGuard parameters(*this);
    expose_qualified_class_facts(declarator);
    bind_function_parameters(declarator);
    return parse_compound();
  }
  bool declarator_denotes_function(const Node &d) const {
    const Node *nested=0;
    bool function_suffix=false;
    for(std::size_t i=0;i<d.children.size();++i) {
      if(d.children[i].text=="parameter-clause") function_suffix=true;
      if(d.children[i].text=="nested-declarator" && !d.children[i].children.empty())nested=&d.children[i].children[0];
    }
    if(nested)return declarator_denotes_function(*nested);
    return function_suffix;
  }
  Node parse_declaration_tail(const Node &specs, bool member) {
    if (member && at(":")) {
      Node bf = n("bit-field-declaration");
      bf.add(specs);
      for (;;) {
        Node bit = n("bit-field-declarator");
        if (!at(":"))
          bit.add(parse_declarator());
        need(":");
        bit.add(parse_expression(2));
        bf.add(bit);
        if (!take(","))
          break;
      }
      need(";");
      return bf;
    }
    if (take(";")) {
      Node x = n("simple-declaration");
      x.add(specs);
      return x;
    }
    std::size_t save = p_;
    Node d;
    try {
      d = parse_declarator();
    } catch (...) {
      p_ = save;
      throw;
    }
    std::string name = declarator_name(d);
    if (member && at(":")) {
      Node bf = n("bit-field-declaration");
      bf.add(specs);
      for (;;) {
        Node bit = n("bit-field-declarator");
        bit.add(d);
        need(":");
        bit.add(parse_expression(2));
        bf.add(bit);
        if (!take(","))
          break;
        d = parse_declarator();
      }
      need(";");
      return bf;
    }
    bool direct_function=false,nested_declarator=false;
    for(std::size_t i=0;i<d.children.size();++i) {
      if(d.children[i].text=="parameter-clause")direct_function=true;
      if(d.children[i].text=="nested-declarator")nested_declarator=true;
    }
    const bool function_declarator=semantic_mode_ ? declarator_denotes_function(d) : direct_function&&!nested_declarator;
    if (at("try") && function_declarator) {
      ++p_;
      ScopeGuard parameters(*this);
      expose_qualified_class_facts(d);
      bind_function_parameters(d);
      Node fn = n("function-definition");
      fn.add(specs);
      fn.add(d);
      Node ft = n("function-try-block");
      if (at(":"))
        ft.add(parse_ctor_initializer());
      ft.add(parse_compound());
      while (take("catch")) {
        Node h = n("handler");
        need("(");
        if (take("...")) {
          Node ex = n("exception-declaration");
          ex.add(n("ellipsis ..."));
          h.add(ex);
        } else {
          Node ex = n("exception-declaration");
          if (!at(")")) {
            ex.add(parse_decl_specs());
            if (!at(")"))
              ex.add(parse_declarator(true));
          }
          h.add(ex);
        }
        need(")");
        h.add(parse_compound());
        ft.add(h);
      }
      fn.add(ft);
      return fn;
    }
    if (at("{") && function_declarator) {
      if (member && !class_names_.empty() &&
          (name == current_class_simple() ||
           name == "~" + current_class_simple())) {
        Node sp = n("special-member-definition " + name);
        sp.add(d);
        sp.add(parse_compound());
        return sp;
      }
      if (member && !class_names_.empty() &&
          (name == current_class_simple() ||
           name == "~" + current_class_simple())) {
        Node sp = n("special-member-definition " + name);
        sp.add(d);
        if (at(":"))
          sp.add(parse_ctor_initializer());
        sp.add(parse_compound());
        return sp;
      }
      Node fn = n("function-definition");
      fn.add(specs);
      fn.add(d);
      fn.add(parse_function_compound(d));
      return fn;
    }
    Node simple = n("simple-declaration");
    simple.add(specs);
    Node list = n("init-declarator-list");
    for (;;) {
      Node item = n("init-declarator");
      item.add(d);
      // For declarations with a function suffix, `= default/delete` has its own
      // node.
      bool direct_fn=false,nested_fn=false;
      for(std::size_t i=0;i<d.children.size();++i) {
        if(d.children[i].text=="parameter-clause")direct_fn=true;
        if(d.children[i].text=="nested-declarator")nested_fn=true;
      }
      bool fn=semantic_mode_ ? declarator_denotes_function(d) : direct_fn&&!nested_fn;
      if (fn && take("=")) {
        std::string v = s();
        if (v == "default" || v == "delete") {
          ++p_;
          Node init = n("initializer");
          init.add(n("special-initializer " + v));
          item.add(init);
        } else {
          Node init = n("initializer");
          init.add(parse_expression(2));
          item.add(init);
        }
      } else if (at("=") || at("{") || at("(")) {
        if (!(fn && at("(")))
          item.add(parse_initializer());
      }
      if (specs.children.size() &&
          specs.children[0].text.find("KW_TYPEDEF") != std::string::npos)
        declare_type(name);
      else
        declare_value(name);
      list.add(item);
      if (!take(","))
        break;
      d = parse_declarator();
      name = declarator_name(d);
    }
    simple.add(list);
    need(";");
    return simple;
  }
  std::string current_class_simple() const {
    if (class_names_.empty())
      return std::string();
    std::string x = class_names_.back();
    std::size_t p = x.find('<');
    if (p != std::string::npos)
      x = x.substr(0, p);
    std::size_t q = x.rfind("::");
    if (q != std::string::npos)
      x = x.substr(q + 2);
    return x;
  }
  bool starts_special_member(bool member) const {
    if (!member)
      return false;
    if (at("~") || at("operator"))
      return true;
    return !class_names_.empty() && identifier() &&
           s() == current_class_simple() && s(1) == "(";
  }
  Node parse_ctor_initializer() {
    need(":");
    Node c = n("ctor-initializer");
    for (;;) {
      Node m = n("mem-initializer");
      std::string id;
      if (at("decltype")) {
        std::size_t start = p_;
        parse_decltype();
        id = compact_range(start, p_);
      } else
        id = parse_name();
      m.add(n("mem-initializer-id " + id));
      if (take("(")) {
        Node args = n("paren-argument-list");
        if (!at(")")) {
          do {
            args.add(parse_expression(2));
          } while (take(","));
        }
        need(")");
        m.add(args);
      } else if (at("{"))
        m.add(parse_braced_init());
      else
        throw std::runtime_error("member initializer expected");
      if (take("..."))
        m.add(n("pack-expansion OP_DOTS:..."));
      c.add(m);
      if (!take(","))
        break;
    }
    return c;
  }
  Node parse_special_member(bool definition) {
    (void)definition;
    std::string name = parse_name();
    std::size_t op = name.find("::operator");
    if (op != std::string::npos && op + 10 < name.size() &&
        name[op + 10] != ' ' && name.substr(op + 10, 2) != "()" &&
        name.substr(op + 10, 2) != "[]")
      name.insert(op + 10, " ");
    Node d = n("declarator");
    d.add(n("identifier " + name));
    d.add(parse_parameter_clause());
    while (at("const") || at("volatile") || at("&") || at("&&") ||
           at("override") || at("final") || at("noexcept")) {
      if (at("override") || at("final")) {
        std::string v = s();
        ++p_;
        d.add(n("virt-specifier " + label_token(v)));
      } else if (take("noexcept"))
        d.add(n("function-qualifier noexcept"));
      else {
        std::string v = s();
        ++p_;
        d.add(n((v == "const" || v == "volatile" ? "cv-qualifier "
                                                 : "function-qualifier ") +
                label_token(v)));
      }
    }
    if (at(":")) {
      ScopeGuard parameters(*this);
      expose_qualified_class_facts(d);
      bind_function_parameters(d);
      Node f = n("special-member-definition " + name);
      f.add(d);
      f.add(parse_ctor_initializer());
      f.add(parse_compound());
      return f;
    }
    if (at("{")) {
      ScopeGuard parameters(*this);
      expose_qualified_class_facts(d);
      bind_function_parameters(d);
      Node f = n("special-member-definition " + name);
      f.add(d);
      f.add(parse_compound());
      return f;
    }
    if(take("=")) {
      std::string value=s();
      if(value!="default" && value!="delete")throw std::runtime_error("invalid special-member initializer");
      ++p_;
      need(";");
      Node f=n("special-member-definition "+name);
      f.add(d);
      f.add(n("special-initializer "+value));
      return f;
    }
    Node f = n("special-member-declaration " + name);
    f.add(d);
    need(";");
    return f;
  }
  Node parse_translation_unit() {
    Node tu = n("translation-unit");
    while (!eof()) {
      std::size_t before = p_;
      try {
        tu.add(parse_declaration());
      } catch (const std::exception &e) {
        std::ostringstream m;
        m << e.what() << " at token " << p_ << " [" << s() << "] near";
        for (std::size_t q = p_ > 5 ? p_ - 5 : 0; q < p_ + 4 && q < t_.size();
             ++q)
          m << " " << q << ":" << t_[q].s;
        throw std::runtime_error(m.str());
      }
      if (p_ == before)
        throw std::runtime_error("declaration made no progress");
    }
    return tu;
  }

public:
  explicit Parser(PostTokenBuffer &input, std::size_t first,
                  std::size_t last, SyntaxArena &arena, bool semantic_mode)
      : arena_(arena), t_(input, first, last), p_(0),
        template_lookahead_work_(0), template_lookahead_budget_(0),
        tokens_(input), qualified_types_(tokens_),
        qualified_templates_(tokens_), force_type_names_(0),
        active_template_declarations_(0), semantic_mode_(semantic_mode) {
    if(semantic_mode_)index_braces();
    const std::size_t max_size = static_cast<std::size_t>(-1);
    template_lookahead_budget_ =
        t_.size() > max_size / 4 ? max_size : t_.size() * 4;
    types_.push_back(FlatNameSet(tokens_));
    values_.push_back(FlatNameSet(tokens_));
    templates_.push_back(FlatNameSet(tokens_));
    for (std::size_t i = first; i < last; ++i)
      if (input[i].kind == PostTokenInvalid)
        throw std::runtime_error("invalid post-token");
  }
  Node parse() { return parse_translation_unit(); }
};
} // namespace

SyntaxTree parse_syntax_tree(PostTokenBuffer &tokens, std::size_t first,
                             std::size_t last, bool semantic_mode) {
  SyntaxTree tree;
  tree.arena.reset(new SyntaxArena());
  Parser parser(tokens, first, last, *tree.arena, semantic_mode);
  tree.root = parser.parse();
  tree.source_files = tokens.take_source_files();
  return tree;
}
