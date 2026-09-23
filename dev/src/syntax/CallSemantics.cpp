#include "syntax/SemanticTypes.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <deque>
#include <map>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
using Node = SyntaxNode;

enum { T_BUILTIN=SEMANTIC_BUILTIN, T_CLASS=SEMANTIC_CLASS,
       T_ENUM=SEMANTIC_ENUM, T_CV=SEMANTIC_CV, T_POINTER=SEMANTIC_POINTER,
       T_LREF=SEMANTIC_LVALUE_REFERENCE, T_RREF=SEMANTIC_RVALUE_REFERENCE,
       T_ARRAY=SEMANTIC_ARRAY, T_FUNCTION=SEMANTIC_FUNCTION,
       T_MEMBER_POINTER=SEMANTIC_MEMBER_POINTER };

struct CType {
  int kind;
  // Nonnegative only for a borrowed projection of an immutable SemanticTypeId.
  // Synthetic conversions clear this key and use structural comparison.
  int canonical_id;
  int template_parameter_id;
  std::string name;
  CType *base;
  CType *enum_underlying;
  std::vector<CType> params;
  unsigned long long bound;
  int owner;
  bool is_const, is_volatile, scoped, variadic;
  CType() : kind(T_BUILTIN), canonical_id(-1), template_parameter_id(-1),
            base(0), enum_underlying(0), bound(0), owner(-1), is_const(false),
            is_volatile(false), scoped(false), variadic(false) {}
};

struct BindingRef {
  int scope, index;
  SemanticBindingInfo value;
  BindingRef() : scope(-1), index(-1) {}
  bool valid() const { return scope >= 0 && index >= 0; }
};

struct ExprInfo {
  CType type;
  std::string category;
  std::vector<BindingRef> functions;
  bool has_constant, null_pointer_constant;
  long long constant;
  ExprInfo() : category("prvalue"), has_constant(false),
               null_pointer_constant(false), constant(0) {}
};

struct Conversion {
  bool viable;
  int rank;
  bool reference_binding;
  int reference_kind; // 1 = lvalue reference, 2 = rvalue reference
  int qualification_steps;
  bool direct_reference_binding;
  bool binds_rvalue;
  Conversion(bool v=false, int r=99, bool ref=false, int ref_kind=0,
             int qualifications=0, bool direct=false, bool rvalue=false)
      : viable(v), rank(r), reference_binding(ref),
        reference_kind(ref_kind), qualification_steps(qualifications),
        direct_reference_binding(direct), binds_rvalue(rvalue) {}
};

static std::string trim(const std::string &s) {
  std::size_t a=0,b=s.size();
  while(a<b && std::isspace(static_cast<unsigned char>(s[a])))++a;
  while(b>a && std::isspace(static_cast<unsigned char>(s[b-1])))--b;
  return s.substr(a,b-a);
}
static std::string after(const std::string &s,const std::string &prefix) {
  return s.compare(0,prefix.size(),prefix)==0?s.substr(prefix.size()):std::string();
}
static std::string indent(unsigned depth) { return std::string(depth*2,' '); }
static std::string literal_token(const Node &n) { return after(n.text,"literal "); }
static std::string id_name(const Node &n) { return after(n.text,"id-expression "); }
static std::string decl_name(const Node &n) {
  if(n.text.compare(0,11,"identifier ")==0)return n.text.substr(11);
  for(std::size_t i=0;i<n.children.size();++i) {
    std::string v=decl_name(n.children[i]);
    if(!v.empty())return v;
  }
  return std::string();
}
static const Node *child(const Node &n,const std::string &text) {
  for(std::size_t i=0;i<n.children.size();++i)
    if(n.children[i].text==text)return &n.children[i];
  return 0;
}
static std::vector<std::string> split_name(const std::string &name) {
  std::vector<std::string> p;
  std::size_t i=0;
  while(i<name.size()) {
    if(name.compare(i,2,"::")==0){i+=2;continue;}
    std::size_t j=name.find("::",i);
    if(j==std::string::npos)j=name.size();
    p.push_back(name.substr(i,j-i)); i=j;
  }
  return p;
}

struct ScopeLocationKey {
  std::uint64_t location; int kind;
  ScopeLocationKey(std::uint64_t l=0,int k=0):location(l),kind(k){}
  bool operator==(const ScopeLocationKey &other) const {return location==other.location&&kind==other.kind;}
};
struct ScopeLocationHash {
  std::size_t operator()(const ScopeLocationKey &key) const {
    return std::hash<std::uint64_t>()(key.location) ^ (std::hash<int>()(key.kind)+0x9e3779b9U+(key.location<<6)+(key.location>>2));
  }
};

class CallSemantics {
  SemanticModel &model_;
  const Node &root_;
  int global_;
  struct SwitchFrame {
    std::unordered_set<unsigned long long> labels;
    CType condition_type;
    bool has_default;
    SwitchFrame():has_default(false){}
  };
  std::vector<SwitchFrame> switch_frames_;
  std::vector<CType> demanded_constructors_;
  struct TemplateInstantiation { std::string name; CType function; int entity; };
  std::vector<TemplateInstantiation> template_instantiations_;
  struct ScopeNameKey {
    int scope;
    SemanticNameId name;
    ScopeNameKey(int s=-1,SemanticNameId n=UINT32_MAX):scope(s),name(n){}
    bool operator==(const ScopeNameKey &other) const {
      return scope==other.scope && name==other.name;
    }
  };
  struct ScopeNameHash {
    std::size_t operator()(const ScopeNameKey &key) const {
      return std::hash<int>()(key.scope) ^
        (std::hash<SemanticNameId>()(key.name)+0x9e3779b9U+
         (static_cast<std::size_t>(key.scope)<<6));
    }
  };
  std::unordered_map<ScopeNameKey,std::vector<BindingRef>,ScopeNameHash> template_functions_by_scope_name_;
  // A TU-local, immutable-type projection cache. SemanticTypeIds remain the
  // equality keys; CType is just the PA7 formatter/conversion adapter.
  mutable std::deque<CType> type_arena_;
  mutable std::vector<CType> canonical_type_cache_;
  mutable std::vector<unsigned char> canonical_type_cached_;
  std::unordered_map<SemanticNameId,std::vector<BindingRef> > anonymous_union_members_by_name_;
  std::unordered_map<ScopeLocationKey,std::vector<int>,ScopeLocationHash> scopes_by_location_;
  mutable std::map<int,std::string> anonymous_class_names_;
  mutable std::map<int,std::string> anonymous_enum_names_;
  mutable std::map<int,std::string> anonymous_union_names_;
  mutable unsigned anonymous_class_serial_, anonymous_enum_serial_;
  unsigned loop_depth_, switch_depth_;

  std::string qualified_scope_type_name(int scope,bool class_scope) const {
    if(scope<0)return std::string();
    std::vector<std::string> parts;
    int current=scope;
    while(current>=0) {
      SemanticScopeInfo info=model_.scope(current);
      if((class_scope&&info.kind==SEMANTIC_CLASS_SCOPE)||info.kind==SEMANTIC_NAMESPACE) {
        std::string name=model_.name_text(info.name);
        if(name!="<global>"&&name!="<unnamed>"&&!name.empty())parts.push_back(name);
      }
      current=info.parent;
    }
    std::string result;
    for(std::size_t i=parts.size();i>0;--i) {
      if(!result.empty())result+="::";
      result+=parts[i-1];
    }
    return result;
  }
  std::string synthetic_class_name(int owner) const {
    std::map<int,std::string>::const_iterator found=anonymous_class_names_.find(owner);
    if(found!=anonymous_class_names_.end())return found->second;
    std::ostringstream name;name<<"__local_type"<<(++anonymous_class_serial_);
    anonymous_class_names_[owner]=name.str();return name.str();
  }
  std::string synthetic_enum_name(int owner) const {
    std::map<int,std::string>::const_iterator found=anonymous_enum_names_.find(owner);
    if(found!=anonymous_enum_names_.end())return found->second;
    std::ostringstream name;name<<"__anonymous_enum"<<(++anonymous_enum_serial_);
    anonymous_enum_names_[owner]=name.str();return name.str();
  }
  CType *store_type(const CType &type) const {
    type_arena_.push_back(type);
    return &type_arena_.back();
  }
  CType from_type_id(int id) const {
    if(id<0)return CType();
    if(static_cast<std::size_t>(id)>=canonical_type_cache_.size()) {
      const std::size_t needed=static_cast<std::size_t>(id)+1;
      canonical_type_cache_.resize(needed);
      canonical_type_cached_.resize(needed,0);
    }
    if(canonical_type_cached_[static_cast<std::size_t>(id)])
      return canonical_type_cache_[static_cast<std::size_t>(id)];
    SemanticTypeInfo info=model_.type(id);
    CType t; t.kind=static_cast<int>(info.kind); t.canonical_id=id; t.bound=info.array_bound;
    if(info.kind==SEMANTIC_BUILTIN&&info.owner_scope>=0)t.template_parameter_id=id;
    t.is_const=info.is_const; t.is_volatile=info.is_volatile;
    t.scoped=info.scoped; t.variadic=info.variadic; t.owner=info.owner_scope;
    if(info.name!=UINT32_MAX)t.name=model_.name_text(info.name);
    if(info.kind==SEMANTIC_CLASS) {
      std::string raw=t.name;
      if(raw.compare(0,6,"class ")==0)raw=raw.substr(6);
      else if(raw.compare(0,7,"struct ")==0)raw=raw.substr(7);
      else if(raw.compare(0,6,"union ")==0)raw=raw.substr(6);
      if(info.owner_scope>=0) {
        std::map<int,std::string>::const_iterator union_name=anonymous_union_names_.find(info.owner_scope);
        std::map<int,std::string>::const_iterator anonymous=anonymous_class_names_.find(info.owner_scope);
        if(union_name!=anonymous_union_names_.end())raw=union_name->second;
        else if(anonymous!=anonymous_class_names_.end())raw=anonymous->second;
        else if(raw.find("__anonymous_class__")==0)raw=synthetic_class_name(info.owner_scope);
        else {
          std::string q=qualified_scope_type_name(info.owner_scope,true);
          if(!q.empty())raw=q;
        }
      }
      t.name=std::string(info.class_tag==1?"class ":info.class_tag==3?"union ":"struct ")+raw;
    } else if(info.kind==SEMANTIC_ENUM) {
      if(info.owner_scope>=0) {
        if(t.name.find("__anonymous_enum__")==0)t.name=synthetic_enum_name(info.owner_scope);
        else t.name=model_.name_text(model_.scope(info.owner_scope).name);
      }
      if(info.enum_underlying_type>=0)
        t.enum_underlying=store_type(from_type_id(info.enum_underlying_type));
      t.name=std::string(info.scoped?"enum class ":"enum ")+t.name;
    }
    if(info.kind==SEMANTIC_CV || info.kind==SEMANTIC_POINTER ||
       info.kind==SEMANTIC_LVALUE_REFERENCE || info.kind==SEMANTIC_RVALUE_REFERENCE ||
       info.kind==SEMANTIC_ARRAY || info.kind==SEMANTIC_MEMBER_POINTER) {
      t.base=store_type(from_type_id(info.base));
      if(info.kind==SEMANTIC_MEMBER_POINTER && info.owner_scope>=0) {
        SemanticScopeInfo owner=model_.scope(info.owner_scope);
        if(owner.parent>=0) {
          SemanticLookupResult found=model_.lookup_unqualified(owner.parent,owner.name,true,false);
          if(found.scope>=0&&found.binding_index>=0) {
            SemanticBindingInfo binding=model_.binding(found.scope,static_cast<std::size_t>(found.binding_index));
            if(binding.type>=0)t.name=format_type(from_type_id(binding.type));
          }
        }
        if(t.name.empty())t.name="struct "+model_.name_text(owner.name);
      }
    } else if(info.kind==SEMANTIC_FUNCTION) {
      t.base=store_type(from_type_id(info.base));
      for(std::size_t i=0;i<info.parameter_count;++i) {
        CType p=from_type_id(model_.type_parameter(id,i));
        if(p.kind==T_CV&&p.base)p=*p.base;
        if(p.kind==T_ARRAY&&p.base)p=derived(T_POINTER,*p.base);
        else if(p.kind==T_FUNCTION)p=derived(T_POINTER,p);
        t.params.push_back(p);
      }
    }
    canonical_type_cache_[static_cast<std::size_t>(id)]=t;
    canonical_type_cached_[static_cast<std::size_t>(id)]=1;
    return t;
  }
  static CType builtin(const std::string &name) {
    CType t; t.kind=T_BUILTIN; t.name=name; return t;
  }
  CType derived(int kind,const CType &base) const {
    CType t; t.kind=kind; t.base=store_type(base); return t;
  }
  static bool is_ref(const CType &t) { return t.kind==T_LREF || t.kind==T_RREF; }
  static CType referred(const CType &t) { return is_ref(t)&&t.base?*t.base:t; }
  static CType unqualified(CType t) {
    while(t.kind==T_CV && t.base)t=*t.base;
    return t;
  }
  static bool is_integral(const CType &in) {
    CType t=unqualified(in);
    if(t.kind==T_ENUM)return !t.scoped;
    if(t.kind!=T_BUILTIN)return false;
    return t.name=="bool" || t.name=="char" || t.name=="signed char" ||
      t.name=="unsigned char" || t.name=="wchar_t" || t.name=="char16_t" ||
      t.name=="char32_t" || t.name=="short int" || t.name=="unsigned short int" ||
      t.name=="int" || t.name=="unsigned int" || t.name=="long int" ||
      t.name=="unsigned long int" || t.name=="long long int" ||
      t.name=="unsigned long long int";
  }
  static bool is_floating(const CType &in) {
    CType t=unqualified(in);
    return t.kind==T_BUILTIN && (t.name=="float" || t.name=="double" || t.name=="long double");
  }
  static bool is_arithmetic(const CType &t) { return is_integral(t)||is_floating(t); }
  static bool is_void(const CType &in) {
    CType t=unqualified(in); return t.kind==T_BUILTIN && t.name=="void";
  }
  static bool is_bool(const CType &in) {
    CType t=unqualified(in); return t.kind==T_BUILTIN && t.name=="bool";
  }
  static bool is_pointer(const CType &in) {
    CType t=unqualified(in); return t.kind==T_POINTER;
  }
  static std::string format_type(const CType &t) {
    switch(t.kind) {
    case T_BUILTIN: case T_CLASS: case T_ENUM:return t.name;
    case T_CV: {
      std::string q;
      if(t.is_const)q="const";
      if(t.is_volatile)q+=(q.empty()?"":" ")+std::string("volatile");
      return q+" "+(t.base?format_type(*t.base):"<invalid>");
    }
    case T_POINTER:return "pointer to "+(t.base?format_type(*t.base):"<invalid>");
    case T_LREF:return "lvalue-reference to "+(t.base?format_type(*t.base):"<invalid>");
    case T_RREF:return "rvalue-reference to "+(t.base?format_type(*t.base):"<invalid>");
    case T_ARRAY:return "array of "+std::to_string(t.bound)+" "+(t.base?format_type(*t.base):"<invalid>");
    case T_MEMBER_POINTER:return "member-pointer of "+t.name+" to "+(t.base?format_type(*t.base):"<invalid>");
    case T_FUNCTION: {
      std::string r="function of (";
      for(std::size_t i=0;i<t.params.size();++i){if(i)r+=", ";r+=format_type(t.params[i]);}
      if(t.variadic){if(!t.params.empty())r+=", ";r+="...";}
      r+=")";
      if(t.is_const)r+=" const";
      if(t.is_volatile)r+=" volatile";
      return r+" returning "+(t.base?format_type(*t.base):"<invalid>");
    }
    default:return "<invalid>";
    }
  }
  static std::string value_category(const CType &t) {
    if(t.kind==T_LREF)return "lvalue";
    if(t.kind==T_RREF)return "xvalue";
    return "prvalue";
  }
  static CType expression_type(const CType &t) { return referred(t); }
  static bool type_equal(CType a,CType b,bool ignore_top_cv=false) {
    if(ignore_top_cv){a=unqualified(a);b=unqualified(b);}
    if(a.canonical_id>=0 && b.canonical_id>=0)
      return a.canonical_id==b.canonical_id;
    if(a.kind!=b.kind)return false;
    if(a.kind==T_BUILTIN) {
      if(a.template_parameter_id>=0||b.template_parameter_id>=0)
        return a.template_parameter_id==b.template_parameter_id;
      return a.name==b.name;
    }
    if(a.kind==T_CLASS || a.kind==T_ENUM) {
      if(a.owner>=0 && b.owner>=0)
        return a.owner==b.owner && (a.kind!=T_ENUM || a.scoped==b.scoped);
      return a.name==b.name && a.owner==b.owner && a.scoped==b.scoped;
    }
    if(a.kind==T_CV && (a.is_const!=b.is_const || a.is_volatile!=b.is_volatile))return false;
    if(a.kind==T_ARRAY && a.bound!=b.bound)return false;
    if(a.kind==T_FUNCTION) {
      if(a.variadic!=b.variadic || a.params.size()!=b.params.size() ||
         a.is_const!=b.is_const || a.is_volatile!=b.is_volatile)return false;
      if(!a.base || !b.base || !type_equal(*a.base,*b.base))return false;
      for(std::size_t i=0;i<a.params.size();++i)if(!type_equal(a.params[i],b.params[i]))return false;
      return true;
    }
    if(a.kind==T_MEMBER_POINTER && a.owner!=b.owner)return false;
    return a.base && b.base && type_equal(*a.base,*b.base);
  }
  int resolve_scope(const std::string &name,int scope) const {
    if(name.find("::")==std::string::npos)return scope;
    std::vector<std::string> parts=split_name(name);
    if(parts.size()<=1)return scope;
    std::vector<SemanticNameId> path;
    for(std::size_t i=0;i<parts.size();++i) {
      SemanticNameId n=model_.find_name(parts[i]);
      if(n==UINT32_MAX)return -1;
      path.push_back(n);
    }
    return model_.lookup_qualified(scope,path,name.compare(0,2,"::")==0).scope;
  }
  BindingRef lookup(const std::string &name,int scope) const {
    BindingRef out;
    if(name.empty())return out;
    if(name.find("::")==std::string::npos) {
      SemanticNameId id=model_.find_name(name);
      if(id==UINT32_MAX)return out;
      SemanticLookupResult result=model_.lookup_unqualified(scope,id);
      out.scope=result.scope;out.index=result.binding_index;
    } else {
      std::vector<std::string> parts=split_name(name);
      std::vector<SemanticNameId> ids;
      for(std::size_t i=0;i<parts.size();++i) {
        SemanticNameId id=model_.find_name(parts[i]);
        if(id==UINT32_MAX)return out;
        ids.push_back(id);
      }
      SemanticLookupResult result=model_.lookup_qualified(scope,ids,name.compare(0,2,"::")==0);
      out.scope=result.scope;out.index=result.binding_index;
    }
    if(out.valid())out.value=model_.binding(out.scope,static_cast<std::size_t>(out.index));
    return out;
  }
  BindingRef binding_at(int scope,const std::string &name,const SyntaxLocation &location) const {
    BindingRef out;
    SemanticNameId id=model_.find_name(name);
    if(id==UINT32_MAX)return out;
    SemanticLookupResult found=model_.binding_at(scope,id,location);
    if(found.scope>=0&&found.binding_index>=0) {
      out.scope=found.scope;out.index=found.binding_index;
      out.value=model_.binding(found.scope,static_cast<std::size_t>(found.binding_index));
    }
    return out;
  }
  static bool fundamental_type_name(const std::string &name,std::string &canonical) {
    static const char *names[]={"void","bool","char","signed char","unsigned char","char16_t","char32_t","wchar_t","short","short int","unsigned short","unsigned short int","int","unsigned","unsigned int","long","long int","unsigned long","unsigned long int","long long","long long int","unsigned long long","unsigned long long int","float","double","long double","nullptr_t"};
    static const char *types[]={"void","bool","char","signed char","unsigned char","char16_t","char32_t","wchar_t","short int","short int","unsigned short int","unsigned short int","int","unsigned int","unsigned int","long int","long int","unsigned long int","unsigned long int","long long int","long long int","unsigned long long int","unsigned long long int","float","double","long double","nullptr_t"};
    for(std::size_t i=0;i<sizeof(names)/sizeof(*names);++i)if(name==names[i]){canonical=types[i];return true;}
    return false;
  }
  static bool hex_digit(char c) {
    return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
  }
  static unsigned escape_digits(const std::string &text,std::size_t at,
                                unsigned maximum,unsigned base) {
    unsigned count=0;
    while(at+count<text.size()&&count<maximum) {
      const char c=text[at+count];
      const bool valid=base==8?(c>='0'&&c<='7'):base==16?hex_digit(c):(c>='0'&&c<='9');
      if(!valid)break;
      ++count;
    }
    return count;
  }
  static unsigned long long decode_character_value(const std::string &text,
                                                    std::size_t begin,
                                                    std::size_t end) {
    if(begin>=end)return 0;
    if(text[begin]!='\\')return static_cast<unsigned char>(text[begin]);
    if(begin+1>=end)return 0;
    const char c=text[begin+1];
    switch(c) {
    case '\'': return '\''; case '"': return '"'; case '?': return '?';
    case '\\': return '\\'; case 'a': return 7; case 'b': return 8;
    case 'f': return 12; case 'n': return 10; case 'r': return 13;
    case 't': return 9; case 'v': return 11;
    default: break;
    }
    if(c>='0'&&c<='7') {
      unsigned long long value=0;unsigned n=escape_digits(text,begin+1,3,8);
      for(unsigned i=0;i<n;++i)value=value*8+static_cast<unsigned>(text[begin+1+i]-'0');
      return value;
    }
    if(c=='x') {
      unsigned long long value=0;unsigned n=escape_digits(text,begin+2,64,16);
      for(unsigned i=0;i<n;++i) {
        const char d=text[begin+2+i];
        value=value*16+static_cast<unsigned>(d<='9'?d-'0':(d|32)-'a'+10);
      }
      return value;
    }
    if(c=='u'||c=='U') {
      unsigned digits=c=='u'?4:8;unsigned long long value=0;
      for(unsigned i=0;i<digits&&begin+2+i<end;++i) {
        const char d=text[begin+2+i];
        value=value*16+static_cast<unsigned>(d<='9'?d-'0':(d|32)-'a'+10);
      }
      return value;
    }
    return static_cast<unsigned char>(c);
  }
  static std::size_t literal_character_count(const std::string &text,
                                             std::size_t begin,
                                             std::size_t end) {
    std::size_t count=0;
    for(std::size_t i=begin;i<end;) {
      if(text[i]!='\\') {++i;++count;continue;}
      ++i;
      if(i>=end)break;
      const char c=text[i++];
      if(c>='0'&&c<='7')i+=escape_digits(text,i-1,3,8)-1;
      else if(c=='x')i+=escape_digits(text,i,64,16);
      else if(c=='u')i=std::min(end,i+4);
      else if(c=='U')i=std::min(end,i+8);
      ++count;
    }
    return count;
  }
  static unsigned long long string_literal_units(const std::string &text,
                                                  std::size_t begin,
                                                  std::size_t end,
                                                  const std::string &prefix,
                                                  bool raw) {
    if(raw)return static_cast<unsigned long long>(end-begin)+1;
    unsigned long long count=0;
    const bool utf16=prefix.find('u')!=std::string::npos && prefix.find("u8")==std::string::npos;
    const bool utf32=prefix.find('U')!=std::string::npos;
    for(std::size_t i=begin;i<end;) {
      if(text[i]!='\\') {++i;++count;continue;}
      ++i;
      if(i>=end)break;
      const char c=text[i++];
      if(c>='0'&&c<='7')i+=escape_digits(text,i-1,3,8)-1;
      else if(c=='x')i+=escape_digits(text,i,64,16);
      else if(c=='u'||c=='U') {
        const unsigned digits=c=='u'?4:8;
        unsigned long long value=0;
        for(unsigned d=0;d<digits&&i<end;++d,++i) {
          const char x=text[i];
          value=value*16+static_cast<unsigned>(x<='9'?x-'0':(x|32)-'a'+10);
        }
        if(utf16&&value>0xffff)count+=2;
        else if(!utf16&&!utf32&&value>0x7f)count+=value<=0x7ff?2:value<=0xffff?3:4;
        else ++count;
        continue;
      }
      ++count;
    }
    return count+1; // terminating zero code unit
  }
  static std::string integer_literal_type(const std::string &digits,
                                         bool is_unsigned,unsigned longs,
                                         unsigned long long value) {
    const bool decimal=digits.empty() || digits[0]!='0' || digits.size()==1;
    const unsigned long long imax=static_cast<unsigned long long>(std::numeric_limits<int>::max());
    const unsigned long long umax=static_cast<unsigned long long>(std::numeric_limits<unsigned int>::max());
    const unsigned long long lmax=static_cast<unsigned long long>(std::numeric_limits<long>::max());
    const unsigned long long ulmax=static_cast<unsigned long long>(std::numeric_limits<unsigned long>::max());
    const unsigned long long llmax=static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    const unsigned long long ullmax=std::numeric_limits<unsigned long long>::max();
    if(is_unsigned) {
      if(longs==0) {
        if(value<=umax)return "unsigned int";
        if(value<=ulmax)return "unsigned long int";
        return "unsigned long long int";
      }
      if(longs==1) {
        if(value<=ulmax)return "unsigned long int";
        return "unsigned long long int";
      }
      return "unsigned long long int";
    }
    if(longs>=2) {
      if(value<=llmax)return "long long int";
      if(!decimal&&value<=ullmax)return "unsigned long long int";
      throw std::runtime_error("integer literal has no representable type");
    }
    if(longs==1) {
      if(value<=lmax)return "long int";
      if(!decimal&&value<=ulmax)return "unsigned long int";
      if(value<=llmax)return "long long int";
      if(!decimal)return "unsigned long long int";
      throw std::runtime_error("integer literal has no representable type");
    }
    if(value<=imax)return "int";
    if(!decimal&&value<=umax)return "unsigned int";
    if(value<=lmax)return "long int";
    if(!decimal&&value<=ulmax)return "unsigned long int";
    if(value<=llmax)return "long long int";
    if(!decimal)return "unsigned long long int";
    throw std::runtime_error("integer literal has no representable type");
  }
  void append_function_candidate(std::vector<BindingRef> &out,
                                 std::unordered_set<int> &seen,
                                 const BindingRef &candidate) const {
    if(!candidate.valid() || candidate.value.kind!=SEMANTIC_FUNCTION_BINDING)return;
    const int identity=candidate.value.function_identity;
    if(identity>=0 && !seen.insert(identity).second)return;
    if(identity<0) {
      for(std::size_t i=0;i<out.size();++i)
        if(out[i].value.entity==candidate.value.entity &&
           out[i].value.type==candidate.value.type)return;
    }
    out.push_back(candidate);
  }
  int template_owner_scope(const std::string &name,int scope,
                           const BindingRef &ordinary) const {
    if(ordinary.valid())return ordinary.scope;
    std::vector<std::string> parts=split_name(name);
    if(parts.empty())return -1;
    const SemanticNameId id=model_.find_name(parts.back());
    if(id==UINT32_MAX)return -1;
    if(name.find("::")!=std::string::npos) {
      if(parts.size()==1)return global_;
      std::vector<SemanticNameId> prefix;
      for(std::size_t i=0;i+1<parts.size();++i) {
        SemanticNameId component=model_.find_name(parts[i]);
        if(component==UINT32_MAX)return -1;
        prefix.push_back(component);
      }
      const bool absolute=name.compare(0,2,"::")==0;
      SemanticLookupResult parent=prefix.size()==1 ?
          model_.lookup_unqualified(absolute?global_:scope,prefix[0]) :
          model_.lookup_qualified(scope,prefix,absolute);
      if(parent.scope<0 || parent.binding_index<0)return -1;
      SemanticBindingInfo binding=model_.binding(
          parent.scope,static_cast<std::size_t>(parent.binding_index));
      if(binding.target_scope>=0)return binding.target_scope;
      if(binding.type>=0) {
        CType type=unqualified(from_type_id(binding.type));
        if(type.kind==T_CLASS || type.kind==T_ENUM)return type.owner;
      }
      return -1;
    }
    for(int current=scope;current>=0;current=model_.scope(current).parent) {
      if(template_functions_by_scope_name_.find(ScopeNameKey(current,id))!=
         template_functions_by_scope_name_.end())return current;
    }
    return -1;
  }
  std::vector<BindingRef> function_set(const std::string &name,int scope) const {
    std::string lookup_name=name;
    std::size_t angle=lookup_name.find('<');
    if(angle!=std::string::npos)lookup_name=lookup_name.substr(0,angle);
    std::vector<BindingRef> out;
    BindingRef first=lookup(lookup_name,scope);
    // Ordinary declarations hide all outer functions, including templates.
    if(first.valid() && first.value.kind!=SEMANTIC_FUNCTION_BINDING)return out;
    std::unordered_set<int> seen;
    SemanticNameId id=model_.find_name(split_name(lookup_name).back());
    if(id==UINT32_MAX)return out;
    if(first.valid()) {
      std::vector<int> indices=model_.binding_indices_named(first.scope,id);
      for(std::size_t i=0;i<indices.size();++i) {
        const int index=indices[i];
        SemanticBindingInfo b=model_.binding(first.scope,static_cast<std::size_t>(index));
        if(b.kind!=SEMANTIC_FUNCTION_BINDING)continue;
        BindingRef x;x.scope=first.scope;x.index=index;x.value=b;
        append_function_candidate(out,seen,x);
      }
    }
    const int template_owner=template_owner_scope(lookup_name,scope,first);
    if(template_owner>=0) {
      std::unordered_map<ScopeNameKey,std::vector<BindingRef>,ScopeNameHash>::const_iterator found=
          template_functions_by_scope_name_.find(ScopeNameKey(template_owner,id));
      if(found!=template_functions_by_scope_name_.end())
        for(std::size_t i=0;i<found->second.size();++i)
          append_function_candidate(out,seen,found->second[i]);
    }
    return out;
  }
  void build_semantic_indexes() {
    for(std::size_t s=0;s<model_.scope_count();++s) {
      SemanticScopeInfo scope=model_.scope(static_cast<int>(s));
      if(scope.location.valid())scopes_by_location_[ScopeLocationKey(scope.location.packed,static_cast<int>(scope.kind))].push_back(static_cast<int>(s));
      for(std::size_t i=0;i<scope.binding_count;++i) {
        SemanticBindingInfo b=model_.binding(static_cast<int>(s),i);
        if(b.kind==SEMANTIC_FUNCTION_BINDING&&scope.kind==SEMANTIC_TEMPLATE_PARAMETERS) {
          BindingRef ref;ref.scope=static_cast<int>(s);ref.index=static_cast<int>(i);ref.value=b;
          template_functions_by_scope_name_[ScopeNameKey(scope.parent,b.name)].push_back(ref);
        }
        if(b.kind==SEMANTIC_VARIABLE&&scope.kind==SEMANTIC_CLASS_SCOPE&&scope.union_class) {
          BindingRef ref;ref.scope=static_cast<int>(s);ref.index=static_cast<int>(i);ref.value=b;
          anonymous_union_members_by_name_[b.name].push_back(ref);
        }
      }
    }
  }
  bool is_template_function(const BindingRef &function) const {
    return function.valid() && model_.scope(function.scope).kind==SEMANTIC_TEMPLATE_PARAMETERS;
  }
  bool deduce_template_type(const CType &pattern,const CType &actual,
                            std::map<int,CType> &substitutions) const {
    if(pattern.template_parameter_id>=0) {
      std::map<int,CType>::iterator it=substitutions.find(pattern.template_parameter_id);
      if(it==substitutions.end()) {
        substitutions[pattern.template_parameter_id]=actual;return true;
      }
      return type_equal(it->second,actual);
    }
    if(pattern.kind==actual.kind) {
      if(pattern.kind==T_POINTER||pattern.kind==T_LREF||pattern.kind==T_RREF||pattern.kind==T_ARRAY||pattern.kind==T_CV||pattern.kind==T_MEMBER_POINTER) {
        if(pattern.kind==T_ARRAY&&pattern.bound!=actual.bound)return false;
        if(pattern.kind==T_CV&&(pattern.is_const!=actual.is_const||pattern.is_volatile!=actual.is_volatile))return false;
        if(pattern.kind==T_MEMBER_POINTER && pattern.owner!=actual.owner)return false;
        return pattern.base&&actual.base&&deduce_template_type(*pattern.base,*actual.base,substitutions);
      }
      if(pattern.kind==T_FUNCTION) {
        if(pattern.params.size()!=actual.params.size()||pattern.variadic!=actual.variadic||!pattern.base||!actual.base||!deduce_template_type(*pattern.base,*actual.base,substitutions))return false;
        for(std::size_t i=0;i<pattern.params.size();++i)if(!deduce_template_type(pattern.params[i],actual.params[i],substitutions))return false;
        return true;
      }
      return type_equal(pattern,actual);
    }
    return false;
  }
  CType substitute_template_type(const CType &pattern,const std::map<int,CType> &substitutions) const {
    if(pattern.template_parameter_id>=0) {
      std::map<int,CType>::const_iterator found=substitutions.find(pattern.template_parameter_id);
      if(found!=substitutions.end())return found->second;
    }
    CType out=pattern;
    out.canonical_id=-1;
    if(pattern.base)out.base=store_type(substitute_template_type(*pattern.base,substitutions));
    for(std::size_t i=0;i<pattern.params.size();++i)out.params[i]=substitute_template_type(pattern.params[i],substitutions);
    return out;
  }
  CType instantiate_template_function(const BindingRef &binding,const std::string &spelling,
                                      int scope,const std::vector<ExprInfo> *arguments,
                                      const CType *expected) const {
    CType function=from_type_id(binding.value.type);
    if(!is_template_function(binding))return function;
    std::map<int,CType> substitutions;
    SemanticScopeInfo template_scope=model_.scope(binding.scope);
    std::vector<int> parameter_types;
    for(std::size_t i=0;i<template_scope.binding_count;++i) {
      SemanticBindingInfo b=model_.binding(binding.scope,i);
      if(b.kind==SEMANTIC_TYPE)parameter_types.push_back(b.type);
    }
    std::size_t angle=spelling.find('<');
    if(angle!=std::string::npos) {
      std::size_t close=spelling.find('>',angle+1);
      if(close!=std::string::npos) {
        std::vector<std::string> args=split_name(spelling.substr(angle+1,close-angle-1));
        for(std::size_t i=0;i<args.size()&&i<parameter_types.size();++i) {
          BindingRef type_binding=lookup(args[i],scope);
          if(type_binding.valid()&&(type_binding.value.kind==SEMANTIC_TYPE||type_binding.value.kind==SEMANTIC_TYPE_ALIAS))
            substitutions[parameter_types[i]]=from_type_id(type_binding.value.type);
        }
      }
    }
    if(arguments)for(std::size_t i=0;i<arguments->size()&&i<function.params.size();++i)
      deduce_template_type(function.params[i],(*arguments)[i].type,substitutions);
    if(expected) {
      CType target=*expected;if(target.kind==T_POINTER&&target.base)target=*target.base;
      if(target.kind==T_MEMBER_POINTER&&target.base)target=*target.base;
      if(target.kind==T_FUNCTION)
        deduce_template_type(function,target,substitutions);
    }
    return substitute_template_type(function,substitutions);
  }
  void record_template_instantiation(const BindingRef &binding,const CType &function) {
    if(!is_template_function(binding))return;
    TemplateInstantiation item;item.name=canonical_function_name(binding);item.function=function;item.entity=binding.value.entity;
    for(std::size_t i=0;i<template_instantiations_.size();++i)
      if(template_instantiations_[i].entity==item.entity && template_instantiations_[i].name==item.name && type_equal(template_instantiations_[i].function,function))return;
    template_instantiations_.push_back(item);
  }
  CType class_type_for_scope(int class_scope) const {
    if(class_scope<0)return CType();
    SemanticScopeInfo cls=model_.scope(class_scope);
    if(cls.parent>=0) {
      SemanticLookupResult found=model_.lookup_unqualified(cls.parent,cls.name,true,false);
      if(found.scope>=0&&found.binding_index>=0) {
        SemanticBindingInfo b=model_.binding(found.scope,static_cast<std::size_t>(found.binding_index));
        if(b.type>=0) {
          CType type=from_type_id(b.type);
          return type;
        }
      }
    }
    CType fallback;fallback.kind=T_CLASS;
    std::string raw=model_.name_text(cls.name);
    std::map<int,std::string>::const_iterator un=anonymous_union_names_.find(class_scope);
    std::map<int,std::string>::const_iterator an=anonymous_class_names_.find(class_scope);
    if(un!=anonymous_union_names_.end())raw=un->second;
    else if(an!=anonymous_class_names_.end())raw=an->second;
    fallback.name=std::string(cls.union_class?"union ":"class ")+raw;
    fallback.owner=class_scope;return fallback;
  }
  CType function_with_implicit_object(CType type,int owner_scope) const {
    if(type.kind!=T_FUNCTION||owner_scope<0||model_.scope(owner_scope).kind!=SEMANTIC_CLASS_SCOPE)return type;
    CType object=class_type_for_scope(owner_scope);
    object=add_cv(object,type.is_const,type.is_volatile);
    type.params.insert(type.params.begin(),derived(T_POINTER,object));
    type.is_const=false;type.is_volatile=false;type.canonical_id=-1;
    return type;
  }
  std::string canonical_function_name(const BindingRef &b) const {
    std::string name=model_.name_text(b.value.name);
    int s=b.scope;
    if(b.value.entity>=0) {
      SemanticEntityInfo e=model_.entity(b.value.entity);
      if(e.name!=UINT32_MAX)name=model_.name_text(e.name);
      if(e.owner_scope>=0)s=e.owner_scope;
    }
    std::vector<std::string> owners;
    while(s>=0) {
      SemanticScopeInfo info=model_.scope(s);
      if(info.kind==SEMANTIC_NAMESPACE && info.parent>=0) {
        std::string n=model_.name_text(info.name);
        if(n!="<unnamed>" && !n.empty())owners.push_back(n);
      } else if(info.kind==SEMANTIC_CLASS_SCOPE) {
        std::string n=model_.name_text(info.name);
        if(!n.empty())owners.push_back(n);
      }
      s=info.parent;
    }
    std::string q;
    for(std::size_t i=owners.size();i>0;--i){if(!q.empty())q+="::";q+=owners[i-1];}
    return q.empty()?name:q+"::"+name;
  }
  BindingRef injected_union_member(const std::string &name,int scope,const SyntaxLocation &use) const {
    SemanticNameId id=model_.find_name(name);if(id==UINT32_MAX)return BindingRef();
    std::unordered_map<SemanticNameId,std::vector<BindingRef> >::const_iterator found=anonymous_union_members_by_name_.find(id);
    if(found==anonymous_union_members_by_name_.end())return BindingRef();
    BindingRef best;std::size_t latest=0;
    for(std::size_t i=0;i<found->second.size();++i) {
      const BindingRef &candidate=found->second[i];
      SemanticScopeInfo owner=model_.scope(candidate.scope);
      if(use.valid()&&owner.location.valid()&&owner.location.file_id()==use.file_id()&&owner.location.source_offset()>use.source_offset())continue;
      if(!best.valid()||owner.location.source_offset()>=latest){best=candidate;latest=owner.location.source_offset();}
    }
    return best;
  }
  int global_scope() const {
    for(std::size_t i=0;i<model_.scope_count();++i) {
      SemanticScopeInfo s=model_.scope(static_cast<int>(i));
      if(s.kind==SEMANTIC_NAMESPACE && s.parent<0)return static_cast<int>(i);
    }
    return 0;
  }
  int scope_at(const Node &node,SemanticScopeKind kind,int parent=-2) const {
    if(!node.location.valid())return -1;
    std::unordered_map<ScopeLocationKey,std::vector<int>,ScopeLocationHash>::const_iterator found=
      scopes_by_location_.find(ScopeLocationKey(node.location.packed,static_cast<int>(kind)));
    if(found==scopes_by_location_.end())return -1;
    for(std::size_t i=0;i<found->second.size();++i) {
      int id=found->second[i];
      if(parent!=-2 && model_.scope(id).parent!=parent)continue;
      return id;
    }
    return -1;
  }
  int namespace_scope(int parent,const std::string &name) const {
    SemanticNameId id=model_.find_name(name);
    if(id!=UINT32_MAX) {
      SemanticLookupResult result=model_.lookup_unqualified(parent,id,false,true);
      if(result.scope>=0&&result.binding_index>=0)return model_.binding(result.scope,static_cast<std::size_t>(result.binding_index)).target_scope;
    }
    return -1;
  }
  CType parameter_type(const CType &function,std::size_t i) const {
    if(function.kind!=T_FUNCTION || i>=function.params.size())return CType();
    return function.params[i];
  }
  static CType function_value(CType type) {
    type=expression_type(type);
    if(type.kind==T_CV && type.base)type=*type.base;
    return type;
  }
  static bool has_cv(const CType &type,bool c,bool v) {
    if(type.kind==T_CV)return (!c||type.is_const) && (!v||type.is_volatile);
    return !c&&!v;
  }
  CType add_cv(const CType &type,bool c,bool v) const {
    if(!c&&!v)return type;
    if(type.kind==T_CV) {
      CType copy=type;
      const bool new_const=copy.is_const||c,new_volatile=copy.is_volatile||v;
      if(new_const!=copy.is_const||new_volatile!=copy.is_volatile)copy.canonical_id=-1;
      copy.is_const=new_const;copy.is_volatile=new_volatile;return copy;
    }
    CType out=derived(T_CV,type);out.is_const=c;out.is_volatile=v;return out;
  }
  static CType promote(CType t) {
    t=unqualified(t);
    if(t.kind==T_ENUM) {
      if(t.scoped)return t;
      if(t.enum_underlying) {
        CType underlying=unqualified(*t.enum_underlying);
        return integer_rank(underlying)<3?builtin("int"):underlying;
      }
      return builtin("int");
    }
    if(t.kind!=T_BUILTIN)return t;
    if(t.name=="bool" || t.name=="char" || t.name=="signed char" ||
       t.name=="unsigned char" || t.name=="short int" || t.name=="unsigned short int" ||
       t.name=="wchar_t" || t.name=="char16_t" || t.name=="char32_t")return builtin("int");
    return t;
  }
  static int integer_rank(const CType &in) {
    CType t=unqualified(in);
    if(t.kind==T_ENUM)return 3;
    if(t.kind!=T_BUILTIN)return -1;
    if(t.name=="bool")return 0;
    if(t.name=="char"||t.name=="signed char"||t.name=="unsigned char"||t.name=="wchar_t"||t.name=="char16_t"||t.name=="char32_t")return 1;
    if(t.name=="short int"||t.name=="unsigned short int")return 2;
    if(t.name=="int"||t.name=="unsigned int")return 3;
    if(t.name=="long int"||t.name=="unsigned long int")return 4;
    if(t.name=="long long int"||t.name=="unsigned long long int")return 5;
    return -1;
  }
  static bool unsigned_type(const CType &in) {
    CType t=unqualified(in);
    return t.name.find("unsigned")==0;
  }
  static CType usual_arithmetic(CType a,CType b) {
    a=unqualified(a);b=unqualified(b);
    if(is_floating(a)||is_floating(b)) {
      if((a.kind==T_BUILTIN&&a.name=="long double")||(b.kind==T_BUILTIN&&b.name=="long double"))return builtin("long double");
      if((a.kind==T_BUILTIN&&a.name=="double")||(b.kind==T_BUILTIN&&b.name=="double"))return builtin("double");
      return builtin("float");
    }
    a=promote(a);b=promote(b);
    int ar=integer_rank(a),br=integer_rank(b);
    if(ar<0||br<0)return builtin("int");
    if(ar>br)return a;
    if(br>ar)return b;
    if(type_equal(a,b))return a;
    return unsigned_type(a)?a:b;
  }
  static bool same_unqualified(CType a,CType b) {
    return type_equal(unqualified(a),unqualified(b));
  }
  bool derived_from(const CType &derived_type,const CType &base_type) const {
    CType d=unqualified(derived_type),b=unqualified(base_type);
    if(d.kind!=T_CLASS||b.kind!=T_CLASS)return false;
    if(type_equal(d,b))return true;
    std::vector<int> work;
    if(d.owner<0)return false;
    SemanticScopeInfo s=model_.scope(d.owner);
    for(std::size_t i=0;i<s.base_count;++i)work.push_back(model_.scope_base(d.owner,i));
    std::vector<int> seen;
    while(!work.empty()) {
      int id=work.back();work.pop_back();
      if(std::find(seen.begin(),seen.end(),id)!=seen.end())continue;
      seen.push_back(id);
      CType t=from_type_id(id);
      if(type_equal(unqualified(t),b))return true;
      if(t.owner>=0) {
        SemanticScopeInfo q=model_.scope(t.owner);
        for(std::size_t i=0;i<q.base_count;++i)work.push_back(model_.scope_base(t.owner,i));
      }
    }
    return false;
  }
  static bool qualification_convert(CType from,CType to,int depth=0,bool added=false,
                                    bool const_intermediate=false) {
    if(type_equal(from,to))return true;
    if(from.kind==T_CV || to.kind==T_CV) {
      CType fb=from.kind==T_CV&&from.base?*from.base:from;
      CType tb=to.kind==T_CV&&to.base?*to.base:to;
      bool fc=from.kind==T_CV&&from.is_const, fv=from.kind==T_CV&&from.is_volatile;
      bool tc=to.kind==T_CV&&to.is_const, tv=to.kind==T_CV&&to.is_volatile;
      if((fc&&!tc)||(fv&&!tv))return false;
      bool now=added||(!fc&&tc)||(!fv&&tv);
      if(depth>=2&&now&&!const_intermediate)return false;
      return type_equal(fb,tb) || qualification_convert(fb,tb,depth,now,
                    const_intermediate || (depth>0&&tc));
    }
    if(from.kind==T_POINTER && to.kind==T_POINTER && from.base && to.base) {
      return qualification_convert(*from.base,*to.base,depth+1,added,const_intermediate);
    }
    if(from.kind==T_ARRAY&&to.kind==T_ARRAY&&from.bound==to.bound&&from.base&&to.base)
      return qualification_convert(*from.base,*to.base,depth,added,const_intermediate);
    return false;
  }
  Conversion conversion(const ExprInfo &source,const CType &target) const {
    CType to=target;
    CType from=source.type;
    if(to.kind==T_LREF || to.kind==T_RREF) {
      const bool lref=to.kind==T_LREF;
      const int ref_kind=lref?1:2;
      CType referent=to.base?*to.base:CType();
      CType src=expression_type(from);
      const bool direct_category=lref ? source.category=="lvalue" : source.category!="lvalue";
      if(direct_category) {
        if(type_equal(src,referent))
          return Conversion(true,0,true,ref_kind,0,true,source.category!="lvalue");
        if(qualification_convert(src,referent))
          return Conversion(true,0,true,ref_kind,1,true,source.category!="lvalue");
        if(unqualified(src).kind==T_CLASS&&unqualified(referent).kind==T_CLASS&&derived_from(src,referent)) {
          bool source_const=src.kind==T_CV&&src.is_const,source_volatile=src.kind==T_CV&&src.is_volatile;
          bool target_const=referent.kind==T_CV&&referent.is_const,target_volatile=referent.kind==T_CV&&referent.is_volatile;
          if((source_const&&!target_const)||(source_volatile&&!target_volatile))return Conversion();
          return Conversion(true,3,true,ref_kind,(target_const&&!source_const)||(target_volatile&&!source_volatile)?1:0,true,source.category!="lvalue");
        }
        if(src.kind==T_POINTER&&referent.kind==T_POINTER&&src.base&&referent.base&&qualification_convert(src,referent))
          return Conversion(true,0,true,ref_kind,1,true,source.category!="lvalue");
      }
      if(has_cv(referent,true,false)) {
        ExprInfo materialized=source;materialized.type=src;materialized.category="prvalue";materialized.functions.clear();
        Conversion c=conversion(materialized,unqualified(referent));
        if(c.viable && !(!lref&&source.category=="lvalue"&&c.rank==0)) {
          const bool direct_temporary=source.category!="lvalue" && c.rank==0 &&
              type_equal(unqualified(src),unqualified(referent));
          return Conversion(true,c.rank,true,ref_kind,c.qualification_steps,
                            direct_temporary,true);
        }
      }
      return Conversion();
    }
    if(is_ref(from))from=referred(from);
    // Array and function lvalues undergo their standard decay conversions.
    if(from.kind==T_ARRAY && from.base)from=derived(T_POINTER,*from.base);
    else if(from.kind==T_FUNCTION)from=derived(T_POINTER,from);
    from=unqualified(from);to=unqualified(to);
    if(type_equal(from,to))return Conversion(true,0);
    if(source.functions.size() && to.kind==T_POINTER && to.base && to.base->kind==T_FUNCTION) {
      for(std::size_t i=0;i<source.functions.size();++i) {
        CType fn=from_type_id(source.functions[i].value.type);
        if(type_equal(fn,*to.base))return Conversion(true,0);
      }
      return Conversion();
    }
    if(is_integral(from)&&is_integral(to)) {
      CType p=promote(from);
      if(type_equal(p,to))return Conversion(true,2);
      return Conversion(true,3);
    }
    if(is_arithmetic(from)&&is_arithmetic(to))return Conversion(true,3);
    if(from.kind==T_POINTER && to.kind==T_POINTER && from.base&&to.base) {
      if(qualification_convert(from,to))return Conversion(true,1);
      CType fb=unqualified(*from.base),tb=unqualified(*to.base);
      if(tb.kind==T_BUILTIN && tb.name=="void" && fb.kind!=T_FUNCTION) {
        bool sc=from.base->kind==T_CV&&from.base->is_const, sv=from.base->kind==T_CV&&from.base->is_volatile;
        bool tc=to.base->kind==T_CV&&to.base->is_const, tv=to.base->kind==T_CV&&to.base->is_volatile;
        if((!sc||tc)&&(!sv||tv))return Conversion(true,3);
      }
      if(fb.kind==T_CLASS && tb.kind==T_CLASS && derived_from(fb,tb)) {
        bool sc=from.base->kind==T_CV&&from.base->is_const, sv=from.base->kind==T_CV&&from.base->is_volatile;
        bool tc=to.base->kind==T_CV&&to.base->is_const, tv=to.base->kind==T_CV&&to.base->is_volatile;
        if((!sc||tc)&&(!sv||tv))return Conversion(true,3,false,0,(tc&&!sc)||(tv&&!sv)?1:0);
      }
    }
    if(from.kind==T_POINTER && to.kind==T_BUILTIN && to.name=="bool")
      return Conversion(true,3,false,0,1);
    if(source.null_pointer_constant && to.kind==T_POINTER)return Conversion(true,3);
    if(source.null_pointer_constant && to.kind==T_BUILTIN&&to.name=="nullptr_t")return Conversion(true,3);
    if(from.kind==T_BUILTIN && from.name=="nullptr_t" && to.kind==T_POINTER)return Conversion(true,3);
    return Conversion();
  }
  static bool better_conversion(const Conversion &a,const Conversion &b,
                                const ExprInfo &source) {
    if(a.rank!=b.rank)return a.rank<b.rank;
    if(a.reference_binding && b.reference_binding) {
      if(a.direct_reference_binding!=b.direct_reference_binding)
        return a.direct_reference_binding;
      if(a.reference_kind!=b.reference_kind && a.binds_rvalue && b.binds_rvalue)
        return a.reference_kind==2;
      if(a.qualification_steps!=b.qualification_steps)
        return a.qualification_steps<b.qualification_steps;
    } else if(!a.reference_binding && !b.reference_binding &&
              a.qualification_steps!=b.qualification_steps) {
      return a.qualification_steps<b.qualification_steps;
    }
    // A by-value lvalue transformation and a reference binding can both be
    // exact-match sequences; neither wins merely because one is a reference.
    return false;
  }
  bool scalar_condition(const ExprInfo &e) const {
    CType t=unqualified(expression_type(e.type));
    return is_arithmetic(t)||t.kind==T_POINTER||(t.kind==T_ENUM&&!t.scoped);
  }
  std::vector<BindingRef> resolve_functions(const Node &node,int scope) const {
    std::string name=id_name(node);
    if(name.empty())return std::vector<BindingRef>();
    return function_set(name,scope);
  }
  ExprInfo literal_expression(const Node &node,unsigned depth,
                              const CType *expected,std::ostream *out) {
    ExprInfo e;
      const std::string tok=literal_token(node);
      CType type;
      bool string_lit=tok.find('"')!=std::string::npos;
      if(string_lit) {
        const std::size_t opening=tok.find('"');
        if(opening==std::string::npos)throw std::runtime_error("malformed string literal");
        const std::string prefix=tok.substr(0,opening);
        const bool raw=prefix.find('R')!=std::string::npos;
        std::size_t begin=opening+1,finish=tok.size();
        if(raw) {
          const std::size_t paren=tok.find('(',opening+1);
          if(paren==std::string::npos)throw std::runtime_error("malformed raw string literal");
          const std::string delimiter=tok.substr(opening+1,paren-opening-1);
          const std::string close=")"+delimiter+"\"";
          const std::size_t end=tok.rfind(close);
          if(end==std::string::npos || end<paren)throw std::runtime_error("malformed raw string literal");
          begin=paren+1;finish=end;
        } else {
          if(tok.size()<=opening+1||tok[tok.size()-1]!='"')throw std::runtime_error("malformed string literal");
          finish=tok.size()-1;
        }
        std::string element="char";
        if(prefix.find('L')!=std::string::npos)element="wchar_t";
        else if(prefix.find('U')!=std::string::npos)element="char32_t";
        else if(prefix.find('u')!=std::string::npos&&prefix.find("u8")==std::string::npos)element="char16_t";
        type=derived(T_ARRAY,add_cv(builtin(element),true,false));
        type.bound=string_literal_units(tok,begin,finish,prefix,raw);
        e.category="lvalue";
      } else if(tok.find('.')!=std::string::npos || tok.find_first_of("pP")!=std::string::npos ||
                ((tok.compare(0,2,"0x")!=0&&tok.compare(0,2,"0X")!=0) &&
                 tok.find_first_of("eE")!=std::string::npos)) {
        type=builtin(tok.size() && (tok[tok.size()-1]=='f'||tok[tok.size()-1]=='F')?"float":
                     tok.size() && (tok[tok.size()-1]=='l'||tok[tok.size()-1]=='L')?"long double":"double");
      } else if(!tok.empty() && (tok[0]=='\'' || tok.compare(0,2,"L'")==0 || tok.compare(0,2,"u'")==0 || tok.compare(0,2,"U'")==0)) {
        const std::size_t opening=tok.find('\''),closing=tok.rfind('\'');
        if(opening==std::string::npos||closing<=opening)throw std::runtime_error("malformed character literal");
        const std::size_t begin=opening+1;
        const std::size_t chars=literal_character_count(tok,begin,closing);
        std::string character_type="int";
        if(tok.compare(0,2,"L'")==0)character_type="wchar_t";
        else if(tok.compare(0,2,"u'")==0)character_type="char16_t";
        else if(tok.compare(0,2,"U'")==0)character_type="char32_t";
        else if(chars==1)character_type="char";
        type=builtin(character_type);
        unsigned long long value=decode_character_value(tok,begin,closing);
        if(value<=static_cast<unsigned long long>(LLONG_MAX)) {
          e.has_constant=true;e.constant=static_cast<long long>(value);
        }
      } else {
        bool uns=false;unsigned longs=0;
        for(std::size_t i=tok.size();i>0;--i) {
          char c=tok[i-1];if(c=='u'||c=='U')uns=true;else if(c=='l'||c=='L')++longs;else break;
        }
        std::string digits=tok;
        while(!digits.empty() && (digits[digits.size()-1]=='u'||digits[digits.size()-1]=='U'||digits[digits.size()-1]=='l'||digits[digits.size()-1]=='L'))digits.erase(digits.size()-1);
        errno=0;char *end=0;unsigned long long v=digits.empty()?0:std::strtoull(digits.c_str(),&end,0);
        if(digits.empty()||errno||end!=digits.c_str()+digits.size())throw std::runtime_error("invalid integer literal");
        type=builtin(integer_literal_type(digits,uns,longs,v));
        if(v<=static_cast<unsigned long long>(LLONG_MAX)){e.has_constant=true;e.constant=static_cast<long long>(v);}
        e.null_pointer_constant=(v==0 && tok.size() && std::isdigit(static_cast<unsigned char>(tok[0])));
      }
      e.type=type;
      if(e.null_pointer_constant && expected && unqualified(*expected).kind==T_POINTER)e.type=unqualified(*expected);
      else if(e.null_pointer_constant && expected && unqualified(*expected).kind==T_BUILTIN&&unqualified(*expected).name=="nullptr_t")e.type=unqualified(*expected);
      else if(expected && type_equal(unqualified(*expected),unqualified(type)) &&
              (expected->kind==T_CV))e.type=*expected;
      if(out)*out<<indent(depth)<<"literal "<<e.category<<" "<<format_type(e.type)<<" "<<tok<<"\n";
      return e;
  }
  ExprInfo expression(const Node &node,int scope,unsigned depth,
                      const CType *expected,std::ostream *out) {
    const std::string &text=node.text;
    ExprInfo e;
    if(text.compare(0,std::string("literal ").size(),"literal ")==0)
      return literal_expression(node,depth,expected,out);
    if(text.compare(0,std::string("keyword-literal ").size(),"keyword-literal ")==0) {
      std::string token=after(text,"keyword-literal ");
      std::string raw=token;
      std::size_t p=raw.find(':');if(p!=std::string::npos)raw=raw.substr(p+1);
      if(raw=="nullptr")e.type=builtin("nullptr_t");else e.type=builtin("bool");
      e.has_constant=true;e.constant=raw=="true"?1:0;
      if(out)*out<<indent(depth)<<"literal prvalue "<<format_type(e.type)<<" "<<token<<"\n";
      return e;
    }
    if(text.compare(0,std::string("id-expression ").size(),"id-expression ")==0) {
      std::string name=id_name(node);
      bool resolved_member_target=false;
      CType selected_member_function;
      std::vector<BindingRef> funcs=function_set(name,scope);
      if(!funcs.empty()) {
        e.functions=funcs;
        BindingRef chosen;
        CType chosen_type;
        if(expected) {
          CType target=*expected;
          if(target.kind==T_LREF||target.kind==T_RREF)target=target.base?*target.base:CType();
          bool member_target=target.kind==T_MEMBER_POINTER;
          CType member_owner;
          if(member_target) {
            member_owner=target.name.empty()?CType():builtin(target.name);
            if(target.base)target=*target.base;
          } else if(target.kind==T_POINTER&&target.base)target=*target.base;
          for(std::size_t i=0;i<funcs.size();++i) {
            CType fn=instantiate_template_function(funcs[i],name,scope,0,&target);
            if(member_target&&fn.kind==T_FUNCTION&&!fn.params.empty()) {
              CType first=fn.params[0];
              if(first.kind==T_POINTER&&first.base &&
                 first.base->name.find(member_owner.name)!=std::string::npos) {
                fn.params.erase(fn.params.begin());
                fn.canonical_id=-1;
              }
            }
            if(type_equal(fn,target)){chosen=funcs[i];chosen_type=fn;break;}
          }
          if(!chosen.valid())throw std::runtime_error("overloaded function does not match target type");
          resolved_member_target=member_target;
          selected_member_function=target;
        } else if(funcs.size()==1) {chosen=funcs[0];chosen_type=instantiate_template_function(chosen,name,scope,0,0);}
        else {
          if(out)throw std::runtime_error("overloaded function id requires target type");
          return e;
        }
        e.type=chosen_type.kind==T_FUNCTION?chosen_type:from_type_id(chosen.value.type);
        if(is_template_function(chosen))record_template_instantiation(chosen,e.type);
        if(resolved_member_target&&e.type.kind==T_FUNCTION) {
          e.type.is_const=selected_member_function.is_const;
          e.type.is_volatile=selected_member_function.is_volatile;
          e.type.canonical_id=-1;
        }
        if(chosen.value.entity>=0)e.type=function_with_implicit_object(e.type,model_.entity(chosen.value.entity).owner_scope);
        e.category="lvalue";e.functions=funcs;
        if(out)*out<<indent(depth)<<"id-expression lvalue "<<format_type(e.type)<<" "<<(name.find('<')==std::string::npos?canonical_function_name(chosen):name)<<"\n";
        return e;
      }
      BindingRef b=lookup(name,scope);
      bool injected_union=false;
      if(!b.valid()) {b=injected_union_member(name,scope,node.location);injected_union=b.valid();}
      if(!b.valid()) {
        if(out)throw std::runtime_error("undeclared identifier: "+name);
        return e;
      }
      if(injected_union) {
        SemanticEntityInfo entity=b.value.entity>=0?model_.entity(b.value.entity):SemanticEntityInfo();
        int owner=entity.owner_scope;
        CType union_type=class_type_for_scope(owner);
        std::string storage=union_storage_name(owner);
        e.type=from_type_id(b.value.type);e.category="lvalue";
        if(out){*out<<indent(depth)<<"member-expression lvalue "<<format_type(e.type)<<" "<<name<<"\n";
          *out<<indent(depth+1)<<"id-expression lvalue "<<format_type(union_type)<<" "<<storage<<"\n";}
        return e;
      }
      if(b.value.entity>=0) {
        SemanticEntityInfo entity=model_.entity(b.value.entity);
        if(entity.owner_scope>=0 && model_.scope(entity.owner_scope).union_class && b.scope!=entity.owner_scope) {
          int union_scope=entity.owner_scope;
          CType union_type=class_type_for_scope(union_scope);
          std::string storage=union_storage_name(union_scope);
          e.type=from_type_id(b.value.type);e.category="lvalue";
          if(out){*out<<indent(depth)<<"member-expression lvalue "<<format_type(e.type)<<" "<<name<<"\n";
            *out<<indent(depth+1)<<"id-expression lvalue "<<format_type(union_type)<<" "<<storage<<"\n";}
          return e;
        }
      }
      if(b.value.kind==SEMANTIC_ENUMERATOR) {
        e.type=from_type_id(b.value.type);e.category="prvalue";
        e.has_constant=b.value.has_constant;e.constant=b.value.constant_value;
        if(out)*out<<indent(depth)<<"literal prvalue "<<format_type(e.type)<<" "<<b.value.constant_value<<"\n";
        return e;
      }
      if(b.value.kind!=SEMANTIC_VARIABLE && b.value.kind!=SEMANTIC_PARAMETER)
        throw std::runtime_error("identifier does not name a value: "+name);
      e.type=from_type_id(b.value.type);e.category="lvalue";
      e.has_constant=b.value.has_constant;e.constant=b.value.constant_value;
      if(out)*out<<indent(depth)<<"id-expression lvalue "<<format_type(expression_type(e.type))<<" "<<name<<"\n";
      return e;
    }
    if(text=="parenthesized-expression") {
      if(node.children.size()!=1)throw std::runtime_error("bad parenthesized expression");
      return expression(node.children[0],scope,depth,expected,out);
    }
    if(text.compare(0,std::string("member-expression ").size(),"member-expression ")==0) {
      if(node.children.size()>1 && node.children[0].text.compare(0,std::string("cast-expression ").size(),"cast-expression ")==0 &&
         node.children[0].children.size()>1 && node.children[0].text.find("OP_LPAREN")!=std::string::npos) {
        CType target=from_type_id(model_.type_from_syntax(node.children[0].children[0],scope));
        Node rewritten("member-expression "+text.substr(std::string("member-expression ").size()));
        rewritten.location=node.location;
        rewritten.add(node.children[0].children[1]);rewritten.add(node.children[1]);
        ExprInfo member_value=member(rewritten,scope,0,0);
        ExprInfo e;e.type=target;e.category="prvalue";
        if(!is_void(target) && !conversion(member_value,target).viable && !is_arithmetic(member_value.type))
          throw std::runtime_error("invalid C-style cast of member expression");
        if(out) {
          std::string cast_op=after(node.children[0].text,"cast-expression ");
          *out<<indent(depth)<<"cast-expression prvalue "<<format_type(target)<<" "<<cast_op<<"\n";
          member(rewritten,scope,depth+1,out);
        }
        return e;
      }
      return member(node,scope,depth,out);
    }
    if(text.compare(0,std::string("call-expression").size(),"call-expression")==0)return call(node,scope,depth,expected,out);
    if(text.compare(0,std::string("unary-expression ").size(),"unary-expression ")==0 || text.compare(0,std::string("postfix-expression ").size(),"postfix-expression ")==0)
      return unary(node,scope,depth,expected,out);
    if(text.compare(0,std::string("binary-expression ").size(),"binary-expression ")==0 || text.compare(0,std::string("assignment-expression ").size(),"assignment-expression ")==0)
      return binary(node,scope,depth,out);
    if(text=="conditional-expression")return conditional(node,scope,depth,out);
    if(text=="subscript-expression") {
      if(node.children.size()!=2)throw std::runtime_error("invalid subscript");
      ExprInfo a=expression(node.children[0],scope,depth+1,0,0);
      ExprInfo b=expression(node.children[1],scope,depth+1,0,0);
      CType at=expression_type(a.type);CType bt=expression_type(b.type);
      CType base;
      if(at.kind==T_ARRAY&&at.base)base=*at.base;
      else if(at.kind==T_POINTER&&at.base)base=*at.base;
      else if(bt.kind==T_ARRAY&&bt.base)base=*bt.base;
      else if(bt.kind==T_POINTER&&bt.base)base=*bt.base;
      else throw std::runtime_error("subscript requires array or pointer");
      bool left_array=at.kind==T_ARRAY||at.kind==T_POINTER;
      bool right_array=bt.kind==T_ARRAY||bt.kind==T_POINTER;
      if(!((left_array&&is_integral(bt))||(right_array&&is_integral(at))))throw std::runtime_error("invalid subscript index");
      e.type=base;e.category="lvalue";
      if(out) {
        *out<<indent(depth)<<"subscript-expression lvalue "<<format_type(e.type)<<"\n";
        if((at.kind==T_ARRAY||at.kind==T_POINTER) || !((bt.kind==T_ARRAY||bt.kind==T_POINTER))) {
          expression(node.children[0],scope,depth+1,0,out);
          expression(node.children[1],scope,depth+1,0,out);
        } else {
          expression(node.children[1],scope,depth+1,0,out);
          expression(node.children[0],scope,depth+1,0,out);
        }
      }
      return e;
    }
    if(text=="sizeof-expression") {
      if(node.children.size()!=1)throw std::runtime_error("sizeof operand missing");
      CType operand;
      if(node.children[0].text=="type-id")operand=from_type_id(model_.type_from_syntax(node.children[0],scope));
      else operand=expression_type(expression(node.children[0],scope,0,0,0).type);
      unsigned long long size=sizeof_type(operand);
      e.type=builtin("unsigned long int");e.has_constant=true;e.constant=static_cast<long long>(size);
      if(out)*out<<indent(depth)<<"sizeof-expression prvalue unsigned long int\n";
      return e;
    }
    if(text=="cast-expression" || text.compare(0,std::string("cast-expression ").size(),"cast-expression ")==0)return cast(node,scope,depth,out);
    throw std::runtime_error("unsupported expression node: "+text);
  }
  ExprInfo member(const Node &node,int scope,unsigned depth,std::ostream *out) {
    if(node.children.size()<2)throw std::runtime_error("member expression malformed");
    ExprInfo object=expression(node.children[0],scope,0,0,0);
    CType raw_object=expression_type(object.type);
    bool const_object=raw_object.kind==T_CV&&raw_object.is_const;
    CType object_type=unqualified(raw_object);
    std::string spelling=after(node.text,"member-expression ");
    bool arrow=spelling.find("OP_ARROW")!=std::string::npos;
    if(arrow) {
      if(object_type.kind!=T_POINTER||!object_type.base)throw std::runtime_error("arrow member access requires pointer");
      CType pointee=*object_type.base;
      const_object=pointee.kind==T_CV&&pointee.is_const;
      object_type=unqualified(pointee);
    }
    if(object_type.kind!=T_CLASS||object_type.owner<0)throw std::runtime_error("member access on non-class object "+format_type(object_type));
    std::string name=after(node.children[1].text,"identifier ");
    std::string qualifier;
    std::size_t q=name.rfind("::");
    if(q!=std::string::npos){qualifier=name.substr(0,q);name=name.substr(q+2);}
    int owner=object_type.owner;
    if(!qualifier.empty()) {
      SemanticScopeInfo c=model_.scope(owner);
      for(std::size_t i=0;i<c.base_count;++i) {
        CType base=from_type_id(model_.scope_base(owner,i));
        if(base.name.find(qualifier)!=std::string::npos){owner=base.owner;break;}
      }
    }
    BindingRef b=lookup(name,owner);
    if(b.valid() && b.scope!=owner)b=BindingRef();
    if(!b.valid() && qualifier.empty()) {
      std::vector<int> work,seen;
      SemanticScopeInfo initial=model_.scope(owner);
      for(std::size_t i=0;i<initial.base_count;++i)work.push_back(model_.scope_base(owner,i));
      while(!work.empty()&&!b.valid()) {
        int base_id=work.back();work.pop_back();
        if(std::find(seen.begin(),seen.end(),base_id)!=seen.end())continue;
        seen.push_back(base_id);
        CType base=from_type_id(base_id);
        if(base.owner>=0) {
          b=lookup(name,base.owner);
          SemanticScopeInfo bs=model_.scope(base.owner);
          for(std::size_t i=0;i<bs.base_count;++i)work.push_back(model_.scope_base(base.owner,i));
        }
      }
    }
    if(!b.valid())throw std::runtime_error("unknown class member: "+name);
    ExprInfo e;e.type=from_type_id(b.value.type);e.category="lvalue";
    if(const_object && b.value.kind==SEMANTIC_VARIABLE)e.type=add_cv(e.type,true,false);
    if(b.value.kind==SEMANTIC_FUNCTION_BINDING)e.functions.push_back(b);
    std::size_t colon=spelling.find(':');
    std::string op=colon==std::string::npos?spelling:spelling.substr(0,colon+1)+name;
    if(out){*out<<indent(depth)<<"member-expression lvalue "<<format_type(expression_type(e.type))<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);}
    return e;
  }
  unsigned long long sizeof_type(CType t) const {
    if(t.kind==T_CV&&t.base)return sizeof_type(*t.base);
    if(t.kind==T_LREF||t.kind==T_RREF) {
      if(!t.base)throw std::runtime_error("invalid reference type in sizeof");
      return sizeof_type(*t.base);
    }
    if(t.kind==T_POINTER)return 8;
    if(t.kind==T_MEMBER_POINTER) {
      if(!t.base)throw std::runtime_error("invalid member pointer type in sizeof");
      return t.base->kind==T_FUNCTION?16:8;
    }
    if(t.kind==T_ARRAY) {
      if(!t.base||t.bound==0)throw std::runtime_error("sizeof incomplete array type");
      const unsigned long long element=sizeof_type(*t.base);
      if(element && t.bound>std::numeric_limits<unsigned long long>::max()/element)
        throw std::runtime_error("array size overflows target size type");
      return t.bound*element;
    }
    if(t.kind==T_ENUM) {
      if(t.enum_underlying)return sizeof_type(*t.enum_underlying);
      return 4;
    }
    if(t.kind==T_CLASS) {
      if(t.owner>=0) {
        SemanticScopeInfo s=model_.scope(t.owner);
        if(s.layout_complete)return static_cast<unsigned long long>(s.object_size>0?s.object_size:1);
      }
      throw std::runtime_error("sizeof incomplete class type");
    }
    if(t.kind==T_FUNCTION || (t.kind==T_BUILTIN&&(t.name=="void"||t.name.empty())))
      throw std::runtime_error("sizeof applied to non-object type");
    if(t.name=="bool"||t.name=="char"||t.name=="signed char"||t.name=="unsigned char")return 1;
    if(t.name=="short int"||t.name=="unsigned short int"||t.name=="char16_t")return 2;
    if(t.name=="long int"||t.name=="unsigned long int"||t.name=="long long int"||
       t.name=="unsigned long long int"||t.name=="double"||t.name=="nullptr_t")return 8;
    if(t.name=="long double")return 16;
    if(t.name=="float"||t.name=="int"||t.name=="unsigned int"||t.name=="wchar_t"||t.name=="char32_t")return 4;
    throw std::runtime_error("sizeof unsupported type");
  }
  static bool constant_truth(const ExprInfo &e) { return e.has_constant&&e.constant!=0; }
  ExprInfo unary(const Node &node,int scope,unsigned depth,const CType *expected,std::ostream *out) {
    if(node.children.size()!=1)throw std::runtime_error("unary operand missing");
    const bool postfix=node.text.compare(0,std::string("postfix-expression ").size(),"postfix-expression ")==0;
    std::string op=after(node.text,postfix?"postfix-expression ":"unary-expression ");
    const CType *operand_target=0;
    if(expected && op.find("OP_AMP:&")!=std::string::npos) {
      if(expected->kind==T_POINTER&&expected->base)operand_target=expected->base;
      else if(expected->kind==T_MEMBER_POINTER)operand_target=expected;
    }
    ExprInfo a=expression(node.children[0],scope,depth+1,operand_target,0);
    CType t=expression_type(a.type);
    if(op.find("OP_AMP:&")!=std::string::npos) {
      ExprInfo e;e.type=derived(T_POINTER,expression_type(a.type));e.category="prvalue";
      if(a.functions.size())e.type=derived(T_POINTER,a.type);
      if(expected&&expected->kind==T_MEMBER_POINTER)e.type=*expected;
      if(out){*out<<indent(depth)<<"unary-expression prvalue "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,operand_target,out);}
      return e;
    }
    if(op.find("OP_STAR:*")!=std::string::npos) {
      if(t.kind==T_ARRAY&&t.base)t=*t.base;
      else if(t.kind==T_POINTER&&t.base)t=*t.base;
      else throw std::runtime_error("dereference requires pointer");
      ExprInfo e;e.type=t;e.category="lvalue";
      if(out){*out<<indent(depth)<<"unary-expression lvalue "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);}
      return e;
    }
    if(op.find("OP_INC")!=std::string::npos||op.find("OP_DEC")!=std::string::npos) {
      if(a.category!="lvalue" || (!is_arithmetic(t)&&t.kind!=T_POINTER))throw std::runtime_error("increment requires modifiable scalar lvalue");
      ExprInfo e;e.type=t;e.category=postfix?"prvalue":"lvalue";
      if(out){*out<<indent(depth)<<(postfix?"postfix-expression ":"unary-expression ")<<e.category<<" "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);}
      return e;
    }
    if(op.find("OP_LNOT")!=std::string::npos) {
      if(!scalar_condition(a))throw std::runtime_error("invalid logical negation");
      ExprInfo e;e.type=builtin("bool");e.category="prvalue";
      if(a.has_constant){e.has_constant=true;e.constant=!a.constant;}
      if(out){*out<<indent(depth)<<"unary-expression prvalue bool "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);}
      return e;
    }
    if(op.find("OP_COMPL")!=std::string::npos) {
      if(!is_integral(t))throw std::runtime_error("bitwise complement requires integer");
      ExprInfo e;e.type=promote(t);e.category="prvalue";
      if(a.has_constant){e.has_constant=true;e.constant=~a.constant;}
      if(out){*out<<indent(depth)<<"unary-expression prvalue "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);}
      return e;
    }
    if(op.find("OP_PLUS")!=std::string::npos||op.find("OP_MINUS")!=std::string::npos) {
      if(!is_arithmetic(t))throw std::runtime_error("unary arithmetic requires arithmetic operand");
      ExprInfo e;e.type=is_integral(t)?promote(t):t;e.category="prvalue";
      if(a.has_constant){e.has_constant=true;e.constant=op.find("OP_MINUS")!=std::string::npos?-a.constant:a.constant;}
      if(out){*out<<indent(depth)<<"unary-expression prvalue "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);}
      return e;
    }
    throw std::runtime_error("unsupported unary operator");
  }
  ExprInfo binary(const Node &node,int scope,unsigned depth,std::ostream *out) {
    if(node.children.size()!=2)throw std::runtime_error("binary operands missing");
    std::string op=after(node.text,node.text.compare(0,std::string("assignment-expression ").size(),"assignment-expression ")==0?"assignment-expression ":"binary-expression ");
    ExprInfo a=expression(node.children[0],scope,depth+1,0,0);
    ExprInfo b=expression(node.children[1],scope,depth+1,0,0);
    CType at=unqualified(expression_type(a.type)),bt=unqualified(expression_type(b.type));
    if(at.kind==T_ARRAY&&at.base)at=derived(T_POINTER,*at.base);
    if(bt.kind==T_ARRAY&&bt.base)bt=derived(T_POINTER,*bt.base);
    if(at.kind==T_FUNCTION)at=derived(T_POINTER,at);
    if(bt.kind==T_FUNCTION)bt=derived(T_POINTER,bt);
    bool assignment=node.text.compare(0,std::string("assignment-expression ").size(),"assignment-expression ")==0;
    if(op.find("OP_COMMA")!=std::string::npos) {
      ExprInfo e=b;
      if(out){*out<<indent(depth)<<"binary-expression "<<e.category<<" "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);expression(node.children[1],scope,depth+1,0,out);}
      return e;
    }
    if(assignment) {
      if(a.category!="lvalue")throw std::runtime_error("assignment left operand is not an lvalue");
      if(op=="OP_ASS:=") {
        Conversion c=conversion(b,at);if(!c.viable)throw std::runtime_error("invalid assignment conversion");
      } else {
        if(!is_arithmetic(at)&&at.kind!=T_POINTER)throw std::runtime_error("invalid compound assignment left operand");
        if(op.find("BANDASS")!=std::string::npos||op.find("BORASS")!=std::string::npos||op.find("XORASS")!=std::string::npos||op.find("LSHIFTASS")!=std::string::npos||op.find("RSHIFTASS")!=std::string::npos) {
          if(!is_integral(at)||!is_integral(bt))throw std::runtime_error("invalid bitwise compound assignment");
        } else if(op.find("PLUSASS")!=std::string::npos && at.kind==T_POINTER) {
          if(!is_integral(bt))throw std::runtime_error("pointer compound assignment requires integral");
        } else if(!is_arithmetic(at)||!is_arithmetic(bt))throw std::runtime_error("invalid compound assignment operands");
      }
      ExprInfo e;e.type=at;e.category="lvalue";
      if(out){*out<<indent(depth)<<"assignment-expression lvalue "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);emit_converted_value(node.children[1],scope,depth+1,at,out);}
      return e;
    }
    CType result;
    bool comparison=op.find("OP_EQ")!=std::string::npos||op.find("OP_NE")!=std::string::npos||op.find("OP_LT")!=std::string::npos||op.find("OP_GT")!=std::string::npos||op.find("OP_LE")!=std::string::npos||op.find("OP_GE")!=std::string::npos;
    bool logical=op.find("OP_LAND")!=std::string::npos||op.find("OP_LOR")!=std::string::npos;
    if(logical) {
      if(!scalar_condition(a)||!scalar_condition(b))throw std::runtime_error("invalid logical operands");
      result=builtin("bool");
    } else if(comparison) {
      bool valid=false;
      if(is_arithmetic(at)&&is_arithmetic(bt))valid=true;
      else if(at.kind==T_POINTER&&bt.kind==T_POINTER&&at.base&&bt.base&&
              (qualification_convert(at,bt)||qualification_convert(bt,at)))valid=true;
      else if((at.kind==T_POINTER&&(b.null_pointer_constant||(bt.kind==T_BUILTIN&&bt.name=="nullptr_t")))||(bt.kind==T_POINTER&&(a.null_pointer_constant||(at.kind==T_BUILTIN&&at.name=="nullptr_t"))))valid=op.find("OP_EQ")!=std::string::npos||op.find("OP_NE")!=std::string::npos;
      else if(at.kind==T_BUILTIN&&bt.kind==T_BUILTIN&&at.name=="nullptr_t"&&bt.name=="nullptr_t"&&(op.find("OP_EQ")!=std::string::npos||op.find("OP_NE")!=std::string::npos))valid=true;
      else if(at.kind==T_ENUM&&bt.kind==T_ENUM&&type_equal(at,bt))valid=true;
      if(!valid)throw std::runtime_error("invalid comparison operands");
      result=builtin("bool");
    } else if(op.find("OP_PLUS")!=std::string::npos && unqualified(at).kind==T_POINTER && is_integral(bt))result=unqualified(at);
    else if(op.find("OP_PLUS")!=std::string::npos && unqualified(bt).kind==T_POINTER && is_integral(at))result=unqualified(bt);
    else if(op.find("OP_MINUS")!=std::string::npos && unqualified(at).kind==T_POINTER && unqualified(bt).kind==T_POINTER) {
      at=unqualified(at);bt=unqualified(bt);
      if(!at.base||!bt.base||!type_equal(unqualified(*at.base),unqualified(*bt.base)))throw std::runtime_error("incompatible pointer subtraction");
      result=builtin("long int");
    } else if((op.find("OP_LSHIFT")!=std::string::npos||op.find("OP_RSHIFT")!=std::string::npos)) {
      if(!is_integral(at)||!is_integral(bt))throw std::runtime_error("shift requires integral operands");
      result=promote(at);
    } else if(op.find("OP_BOR")!=std::string::npos||op.find("OP_XOR")!=std::string::npos||op.find("OP_BAND")!=std::string::npos||op.find("OP_MOD")!=std::string::npos) {
      if(!is_integral(at)||!is_integral(bt))throw std::runtime_error("bitwise operator requires integral operands");
      result=usual_arithmetic(at,bt);
    } else {
      if(!is_arithmetic(at)||!is_arithmetic(bt))throw std::runtime_error("arithmetic operator requires arithmetic operands");
      result=usual_arithmetic(at,bt);
    }
    ExprInfo e;e.type=result;e.category="prvalue";
    if(a.has_constant&&b.has_constant) {
      e.has_constant=true;
      if(op.find("OP_PLUS")!=std::string::npos)e.constant=a.constant+b.constant;
      else if(op.find("OP_MINUS")!=std::string::npos)e.constant=a.constant-b.constant;
      else if(op.find("OP_STAR")!=std::string::npos)e.constant=a.constant*b.constant;
      else if(op.find("OP_DIV")!=std::string::npos&&b.constant)e.constant=a.constant/b.constant;
      else if(op.find("OP_MOD")!=std::string::npos&&b.constant)e.constant=a.constant%b.constant;
      else if(op.find("OP_EQ")!=std::string::npos)e.constant=a.constant==b.constant;
      else if(op.find("OP_NE")!=std::string::npos)e.constant=a.constant!=b.constant;
      else if(op.find("OP_LT")!=std::string::npos)e.constant=a.constant<b.constant;
      else if(op.find("OP_GT")!=std::string::npos)e.constant=a.constant>b.constant;
      else if(op.find("OP_LE")!=std::string::npos)e.constant=a.constant<=b.constant;
      else if(op.find("OP_GE")!=std::string::npos)e.constant=a.constant>=b.constant;
      else if(op.find("OP_LAND")!=std::string::npos)e.constant=a.constant&&b.constant;
      else if(op.find("OP_LOR")!=std::string::npos)e.constant=a.constant||b.constant;
      else if(op.find("OP_BAND")!=std::string::npos)e.constant=a.constant&b.constant;
      else if(op.find("OP_BOR")!=std::string::npos)e.constant=a.constant|b.constant;
      else if(op.find("OP_XOR")!=std::string::npos)e.constant=a.constant^b.constant;
      else if(op.find("OP_LSHIFT")!=std::string::npos)e.constant=a.constant<<b.constant;
      else if(op.find("OP_RSHIFT")!=std::string::npos)e.constant=a.constant>>b.constant;
      else e.has_constant=false;
    }
    if(out){*out<<indent(depth)<<"binary-expression prvalue "<<format_type(e.type)<<" "<<op<<"\n";expression(node.children[0],scope,depth+1,0,out);expression(node.children[1],scope,depth+1,0,out);}
    return e;
  }
  ExprInfo conditional(const Node &node,int scope,unsigned depth,std::ostream *out) {
    if(node.children.size()!=3)throw std::runtime_error("conditional expression malformed");
    ExprInfo c=expression(node.children[0],scope,depth+1,0,0);
    if(!scalar_condition(c))throw std::runtime_error("conditional test is not scalar");
    ExprInfo a=expression(node.children[1],scope,depth+1,0,0);
    ExprInfo b=expression(node.children[2],scope,depth+1,0,0);
    CType at=expression_type(a.type),bt=expression_type(b.type);
    ExprInfo e;
    if(type_equal(unqualified(at),unqualified(bt)) && a.category=="lvalue" && b.category=="lvalue") {e.type=unqualified(at);e.category="lvalue";}
    else if(type_equal(unqualified(at),unqualified(bt)))e.type=unqualified(at);
    else if(is_bool(at)&&is_bool(bt))e.type=builtin("bool");
    else if((at.kind==T_POINTER&&(b.null_pointer_constant||(bt.kind==T_BUILTIN&&bt.name=="nullptr_t"))))e.type=at;
    else if((bt.kind==T_POINTER&&(a.null_pointer_constant||(at.kind==T_BUILTIN&&at.name=="nullptr_t"))))e.type=bt;
    else if(is_arithmetic(at)&&is_arithmetic(bt))e.type=usual_arithmetic(at,bt);
    else if(at.kind==T_POINTER&&bt.kind==T_POINTER&&at.base&&bt.base) {
      if(qualification_convert(at,bt))e.type=bt;
      else if(qualification_convert(bt,at))e.type=at;
      else throw std::runtime_error("conditional pointer types incompatible");
    } else if(type_equal(unqualified(at),unqualified(bt)))e.type=unqualified(at);
    else throw std::runtime_error("conditional operands have no common type");
    if(e.category!="lvalue")e.category="prvalue";
    if(c.has_constant){ExprInfo chosen=c.constant?a:b;if(chosen.has_constant){e.has_constant=true;e.constant=chosen.constant;}}
    if(out){*out<<indent(depth)<<"conditional-expression "<<e.category<<" "<<format_type(e.type)<<"\n";expression(node.children[0],scope,depth+1,0,out);expression(node.children[1],scope,depth+1,0,out);expression(node.children[2],scope,depth+1,0,out);}
    return e;
  }
  ExprInfo cast(const Node &node,int scope,unsigned depth,std::ostream *out) {
    if(node.children.size()<2)throw std::runtime_error("cast type or expression missing");
    CType target=from_type_id(model_.type_from_syntax(node.children[0],scope));
    const std::string label=after(node.text,"cast-expression ");
    ExprInfo src=expression(node.children[1],scope,depth+1,
                            (label.find("KW_STATIC_CAST")!=std::string::npos)?&target:0,0);
    CType from=unqualified(expression_type(src.type));
    if(!is_void(target)) {
      bool ok=conversion(src,target).viable ||
        ((target.kind==T_LREF||target.kind==T_RREF)&&target.base&&type_equal(from,*target.base)) ||
        (is_arithmetic(from)&&is_arithmetic(target)) ||
        (from.kind==T_POINTER&&target.kind==T_POINTER && from.base&&target.base &&
         (unqualified(*from.base).kind==T_CLASS && unqualified(*target.base).kind==T_CLASS &&
          (derived_from(*from.base,*target.base)||derived_from(*target.base,*from.base)))) ||
        ((from.kind==T_ENUM||is_integral(from))&&(target.kind==T_ENUM||is_integral(target)));
      if(!ok)throw std::runtime_error("invalid explicit cast "+format_type(from)+" to "+format_type(target));
    }
    ExprInfo e;e.type=target;e.category=target.kind==T_LREF?"lvalue":target.kind==T_RREF?"xvalue":"prvalue";
    if(src.has_constant && is_arithmetic(target)){e.has_constant=true;e.constant=src.constant;}
    if(out) {
      if(target.kind==T_MEMBER_POINTER) {
        expression(node.children[1],scope,depth,&target,out);
        return e;
      }
      const bool ref_cast=(target.kind==T_LREF||target.kind==T_RREF) &&
        node.children[1].text.compare(0,14,"id-expression ")==0;
      if(ref_cast) {
        *out<<indent(depth)<<"id-expression "<<e.category<<" "<<format_type(target)<<" "
            <<after(node.children[1].text,"id-expression ")<<"\n";
      } else {
        *out<<indent(depth)<<"cast-expression "<<e.category<<" "<<format_type(e.type)
            <<(label.empty()?std::string():" "+label)<<"\n";
        expression(node.children[1],scope,depth+1,(label.find("KW_STATIC_CAST")!=std::string::npos)?&target:0,out);
      }
    }
    return e;
  }
  ExprInfo call(const Node &node,int scope,unsigned depth,const CType *,std::ostream *out) {
    if(node.children.size()<1)throw std::runtime_error("call has no callee");
    const Node &callee=node.children[0];
    const Node *args=0;
    if(node.children.size()>1)args=&node.children[1];
    std::vector<const Node *> arguments;
    if(args)for(std::size_t i=0;i<args->children.size();++i)arguments.push_back(&args->children[i]);
    std::string direct_name;
    const Node *callee_id=&callee;
    while(callee_id->text=="parenthesized-expression"&&callee_id->children.size()==1)callee_id=&callee_id->children[0];
    if(callee_id->text.compare(0,std::string("id-expression ").size(),"id-expression ")==0)direct_name=id_name(*callee_id);
    if(direct_name=="__builtin_abort") {
      if(!arguments.empty())throw std::runtime_error("__builtin_abort takes no arguments");
      ExprInfo e;e.type=builtin("void");
      if(out){*out<<indent(depth)<<"call-expression prvalue void\n"<<indent(depth+1)<<"callee __builtin_abort function of () returning void\n";}
      return e;
    }
    if(direct_name=="__builtin_constant_p") {
      if(arguments.size()!=1)throw std::runtime_error("__builtin_constant_p requires one argument");
      ExprInfo arg=expression(*arguments[0],scope,0,0,0);
      bool known=arg.has_constant;
      ExprInfo e;e.type=builtin("int");e.has_constant=true;e.constant=known?1:0;
      if(out)*out<<indent(depth)<<"literal prvalue int "<<(known?1:0)<<"\n";
      return e;
    }
    std::vector<BindingRef> candidates;
    if(!direct_name.empty())candidates=function_set(direct_name,scope);
    if(candidates.empty() && !direct_name.empty()) {
      std::string canonical;
      BindingRef type_binding=lookup(direct_name,scope);
      if(fundamental_type_name(direct_name,canonical)) {
        CType target=builtin(canonical);
        if(arguments.size()>1)throw std::runtime_error("functional cast has too many arguments");
        ExprInfo e;e.type=target;e.category="prvalue";
        if(arguments.empty()){if(is_arithmetic(target)){e.has_constant=true;e.constant=0;}}
        else {e=expression(*arguments[0],scope,0,&target,0);e.type=target;e.category="prvalue";}
        if(out){if(arguments.empty()&&is_arithmetic(target))*out<<indent(depth)<<"literal prvalue "<<format_type(target)<<" 0\n";else {*out<<indent(depth)<<"cast-expression prvalue "<<format_type(target)<<"\n";if(!arguments.empty())expression(*arguments[0],scope,depth+1,&target,out);}}
        return e;
      }
      if(direct_name.compare(0,9,"decltype(")==0) {
        std::size_t end=direct_name.rfind(')');
        std::string value=end==std::string::npos?std::string():trim(direct_name.substr(9,end-9));
        if(value.size()>1&&value[0]=='('&&value[value.size()-1]==')')value=trim(value.substr(1,value.size()-2));
        BindingRef source=lookup(value,scope);
        if(source.valid()&&(source.value.kind==SEMANTIC_VARIABLE||source.value.kind==SEMANTIC_PARAMETER)) {
          CType target=expression_type(from_type_id(source.value.type));
          if(arguments.size()>1)throw std::runtime_error("functional cast has too many arguments");
          ExprInfo e;e.type=target;e.category="prvalue";
          if(arguments.empty()){if(is_arithmetic(target)){e.has_constant=true;e.constant=0;}}
          else {e=expression(*arguments[0],scope,0,&target,0);e.type=target;e.category="prvalue";}
          if(out){if(arguments.empty()&&is_arithmetic(target))*out<<indent(depth)<<"literal prvalue "<<format_type(target)<<" 0\n";else {*out<<indent(depth)<<"cast-expression prvalue "<<format_type(target)<<"\n";if(!arguments.empty())expression(*arguments[0],scope,depth+1,&target,out);}}
          return e;
        }
      }
      if(type_binding.valid()&&(type_binding.value.kind==SEMANTIC_TYPE||type_binding.value.kind==SEMANTIC_TYPE_ALIAS)) {
        CType target=from_type_id(type_binding.value.type);
        if(arguments.size()>1)throw std::runtime_error("functional cast has too many arguments");
        ExprInfo e;e.type=target;
        if(arguments.empty()){e.category="prvalue";if(is_arithmetic(target)){e.has_constant=true;e.constant=0;}}
        else {e=expression(*arguments[0],scope,0,&target,0);e.type=target;e.category="prvalue";}
        if(out){if(arguments.empty()&&is_arithmetic(target))*out<<indent(depth)<<"literal prvalue "<<format_type(target)<<" 0\n";else {*out<<indent(depth)<<"cast-expression prvalue "<<format_type(target)<<"\n";if(!arguments.empty())expression(*arguments[0],scope,depth+1,&target,out);}}
        return e;
      }
    }
    if(candidates.empty()) {
      ExprInfo function=expression(callee,scope,depth+1,0,0);
      CType f=function_value(function.type);
      if(f.kind==T_POINTER&&f.base)f=*f.base;
      if(f.kind!=T_FUNCTION)throw std::runtime_error("called expression is not a function");
      if((!f.variadic && arguments.size()!=f.params.size()) || (f.variadic&&arguments.size()<f.params.size()))
        throw std::runtime_error("indirect call argument count mismatch");
      for(std::size_t i=0;i<f.params.size();++i) {
        ExprInfo arg=expression(*arguments[i],scope,0,0,0);
        if(!conversion(arg,f.params[i]).viable)throw std::runtime_error("invalid indirect call argument");
      }
      ExprInfo e;e.type=*f.base;e.category=value_category(e.type);
      if(out){*out<<indent(depth)<<"call-expression "<<e.category<<" "<<format_type(e.type)<<"\n";expression(callee,scope,depth+1,0,out);for(std::size_t i=0;i<arguments.size();++i)emit_call_argument(*arguments[i],scope,depth+1,i<f.params.size()?&f.params[i]:0,out);}
      return e;
    }
    struct Viable { BindingRef fn; CType type; std::vector<Conversion> ranks; };
    std::vector<ExprInfo> actual_arguments;
    for(std::size_t i=0;i<arguments.size();++i)actual_arguments.push_back(expression(*arguments[i],scope,0,0,0));
    std::vector<Viable> viable;
    for(std::size_t i=0;i<candidates.size();++i) {
      CType f=instantiate_template_function(candidates[i],direct_name,scope,&actual_arguments,0);
      if(f.kind!=T_FUNCTION)continue;
      if((!f.variadic&&arguments.size()!=f.params.size())||(f.variadic&&arguments.size()<f.params.size()))continue;
      Viable v;v.fn=candidates[i];v.type=f;bool ok=true;
      for(std::size_t j=0;j<arguments.size();++j) {
        ExprInfo arg=actual_arguments[j];
        if(j<f.params.size()) {
          Conversion c=conversion(arg,f.params[j]);
          if(!c.viable){ok=false;break;}v.ranks.push_back(c);
        } else v.ranks.push_back(Conversion(true,4));
      }
      if(ok)viable.push_back(v);
    }
    if(viable.empty()) {
      throw std::runtime_error("no viable function call");
    }
    std::vector<std::size_t> best;
    for(std::size_t i=0;i<viable.size();++i) {
      bool dominated=false;
      for(std::size_t j=0;j<viable.size()&&!dominated;++j)if(i!=j) {
        bool no_worse=true,better=false;
        for(std::size_t k=0;k<viable[i].ranks.size();++k) {
          const ExprInfo &actual=actual_arguments[k];
          if(better_conversion(viable[i].ranks[k],viable[j].ranks[k],actual))
            no_worse=false;
          if(better_conversion(viable[j].ranks[k],viable[i].ranks[k],actual))
            better=true;
        }
        if(no_worse&&better)dominated=true;
      }
      if(!dominated)best.push_back(i);
    }
    if(best.size()!=1)throw std::runtime_error("ambiguous function call");
    const Viable &selected=viable[best[0]];
    if(is_template_function(selected.fn))record_template_instantiation(selected.fn,selected.type);
    if(out) {
      *out<<indent(depth)<<"call-expression ";
      CType ret=selected.type.base?*selected.type.base:CType();
      std::string cat=value_category(ret);
      *out<<cat<<" "<<format_type(ret)<<"\n";
      *out<<indent(depth+1)<<"callee "<<canonical_function_name(selected.fn)<<" "<<format_type(selected.type)<<"\n";
      for(std::size_t i=0;i<arguments.size();++i) {
        const CType *target=i<selected.type.params.size()?&selected.type.params[i]:0;
        emit_call_argument(*arguments[i],scope,depth+1,target,out);
      }
    }
    ExprInfo e;e.type=selected.type.base?*selected.type.base:CType();e.category=value_category(e.type);
    return e;
  }
  bool pointer_class_conversion(const ExprInfo &source,const CType &target) const {
    CType from=unqualified(expression_type(source.type)),to=unqualified(target);
    if(from.kind!=T_POINTER||to.kind!=T_POINTER||!from.base||!to.base)return false;
    CType fb=unqualified(*from.base),tb=unqualified(*to.base);
    return fb.kind==T_CLASS&&tb.kind==T_CLASS&&!type_equal(fb,tb)&&
      (derived_from(fb,tb)||derived_from(tb,fb));
  }
  void emit_converted_value(const Node &node,int scope,unsigned depth,const CType &target,std::ostream *out) {
    if(!out){expression(node,scope,depth,&target,0);return;}
    ExprInfo source=expression(node,scope,0,0,0);
    if(pointer_class_conversion(source,target)) {
      *out<<indent(depth)<<"cast-expression prvalue "<<format_type(target)<<"\n";
      expression(node,scope,depth+1,0,out);
    } else expression(node,scope,depth,&target,out);
  }
  void emit_call_argument(const Node &argument,int scope,unsigned depth,
                          const CType *parameter,std::ostream *out) {
    if(!out) {expression(argument,scope,depth,parameter,0);return;}
    if(parameter) {
      ExprInfo value=expression(argument,scope,0,0,0);
      if(pointer_class_conversion(value,*parameter)) {
        *out<<indent(depth)<<"cast-expression prvalue "<<format_type(*parameter)<<"\n";
        expression(argument,scope,depth+1,0,out);return;
      }
    }
    if(parameter&&(parameter->kind==T_LREF||parameter->kind==T_RREF)&&parameter->base) {
      ExprInfo source=expression(argument,scope,0,0,0);
      CType from=expression_type(source.type),to=*parameter->base;
      if(source.category=="lvalue"&&unqualified(from).kind==T_CLASS&&unqualified(to).kind==T_CLASS&&derived_from(from,to)) {
        *out<<indent(depth)<<"cast-expression lvalue "<<format_type(to)<<"\n";
        expression(argument,scope,depth+1,0,out);return;
      }
      Conversion material=conversion(source,unqualified(to));
      const bool direct=type_equal(from,to)||qualification_convert(from,to)||
        (from.kind==T_CLASS&&to.kind==T_CLASS&&derived_from(from,to));
      if(material.viable&&!direct) {
        *out<<indent(depth)<<"cast-expression prvalue "<<format_type(to)<<"\n";
        expression(argument,scope,depth+1,0,out);
        return;
      }
    }
    expression(argument,scope,depth,parameter,out);
  }
  bool initializer_node(const Node &item,const Node **init) const {
    for(std::size_t i=0;i<item.children.size();++i)
      if(item.children[i].text=="initializer"){*init=&item.children[i];return true;}
    return false;
  }
  void emit_initialization(const Node &init,int scope,const CType &target,unsigned depth,std::ostream *out) {
    if(init.children.empty())return;
    const Node &value=init.children[0];
    if(value.text=="paren-initializer") {
      if(value.children.size()!=1)throw std::runtime_error("direct initializer arity unsupported");
      ExprInfo x=expression(value.children[0],scope,depth,&target,out);
      if(!conversion(x,target).viable)throw std::runtime_error("invalid direct initialization");
    } else if(value.text=="braced-init-list") {
      CType arr=unqualified(target);
      if(arr.kind==T_ARRAY&&arr.base) {
        if(value.children.size()>arr.bound)throw std::runtime_error("too many array initializers");
        if(out)*out<<indent(depth)<<"braced-init-list lvalue "<<format_type(target)<<"\n";
        for(std::size_t i=0;i<value.children.size();++i) {
          ExprInfo x=expression(value.children[i],scope,depth+1,arr.base,out);
          if(!conversion(x,*arr.base).viable)throw std::runtime_error("invalid array element initializer");
        }
      } else {
        if(value.children.size()>1)throw std::runtime_error("multiple scalar initializers");
        if(!value.children.empty()) {
          ExprInfo x=expression(value.children[0],scope,depth,&target,out);
          if(!conversion(x,target).viable)throw std::runtime_error("invalid list initialization");
        }
      }
    } else {
      ExprInfo x=expression(value,scope,depth,&target,out);
      if(!conversion(x,target).viable)throw std::runtime_error("invalid copy initialization");
    }
  }
  std::string class_short_name(const CType &type) const {
    std::string n=type.name;
    if(n.compare(0,7,"struct ")==0)n=n.substr(7);
    else if(n.compare(0,6,"union ")==0)n=n.substr(6);
    else if(n.compare(0,6,"class ")==0)n=n.substr(6);
    return n;
  }
  void emit_constructor_action(const CType &type,unsigned depth,std::ostream &out,const std::string &variable) {
    CType cls=unqualified(type);
    if(cls.kind!=T_CLASS)return;
    bool recorded=false;
    for(std::size_t i=0;i<demanded_constructors_.size();++i)
      if(type_equal(demanded_constructors_[i],cls))recorded=true;
    if(!recorded)demanded_constructors_.push_back(cls);
    std::string name=class_short_name(cls);
    CType this_pointer=derived(T_POINTER,cls);
    CType ctor;ctor.kind=T_FUNCTION;ctor.name="";ctor.base=store_type(builtin("void"));ctor.params.push_back(this_pointer);
    out<<indent(depth)<<"constructor-action "<<name<<"::"<<name<<"\n";
    out<<indent(depth+1)<<"call-expression prvalue void\n";
    out<<indent(depth+2)<<"callee "<<name<<"::"<<name<<" "<<format_type(ctor)<<"\n";
    out<<indent(depth+2)<<"unary-expression prvalue "<<format_type(this_pointer)<<" OP_AMP:&\n";
    out<<indent(depth+3)<<"id-expression lvalue "<<format_type(cls)<<" "<<variable<<"\n";
  }
  void emit_implicit_constructor_definitions(std::ostream &out) {
    for(std::size_t i=0;i<demanded_constructors_.size();++i) {
      const CType &cls=demanded_constructors_[i];
      std::string name=class_short_name(cls);
      CType this_pointer=derived(T_POINTER,cls);
      CType fn;fn.kind=T_FUNCTION;fn.base=store_type(builtin("void"));fn.params.push_back(this_pointer);
      out<<"  function-definition "<<name<<"::"<<name<<" "<<format_type(fn)<<"\n";
      out<<"    parameter this "<<format_type(this_pointer)<<"\n";
      out<<"    compound-statement\n";
    }
  }
  std::string union_storage_name(int union_scope) const {
    std::string n=model_.name_text(model_.scope(union_scope).name);
    std::map<int,std::string>::const_iterator generated=anonymous_union_names_.find(union_scope);
    if(generated!=anonymous_union_names_.end())n=generated->second;
    else {
      SemanticScopeInfo scope=model_.scope(union_scope);
      if(scope.parent>=0&&model_.scope(scope.parent).kind==SEMANTIC_BLOCK_SCOPE&&scope.location.valid()) {
        std::ostringstream stable;stable<<"__anonymous_union_type__"<<(union_scope+4)<<"_"<<(scope.location.source_offset()>=6?scope.location.source_offset()-6:scope.location.source_offset());
        n=stable.str();anonymous_union_names_[union_scope]=n;
      }
    }
    const std::string prefix="__anonymous_union_type__";
    if(n.compare(0,prefix.size(),prefix)==0)return "__anonymous_union_storage__"+n.substr(prefix.size());
    return "__anonymous_union_storage__";
  }
  void emit_anonymous_union_storage(int union_scope,unsigned depth,std::ostream &out) {
    std::string name=union_storage_name(union_scope);
    CType type=class_type_for_scope(union_scope);
    out<<indent(depth)<<"variable "<<name<<" "<<format_type(type)<<"\n";
    emit_constructor_action(type,depth+1,out,name);
  }
  void emit_simple_declaration(const Node &node,int scope,unsigned depth,std::ostream &out,bool top=false) {
    const Node *spec=child(node,"decl-specifier-seq");
    const Node *list=child(node,"init-declarator-list");
    if(!list) {
      if(!top)out<<indent(depth)<<"simple-declaration\n";
      if(spec)for(std::size_t i=0;i<spec->children.size();++i)if(spec->children[i].text.compare(0,std::string("class-specifier").size(),"class-specifier")==0 &&
           (spec->children[i].text.find("union")!=std::string::npos || (!spec->children[i].children.empty()&&spec->children[i].children[0].text.find("KW_UNION")!=std::string::npos))) {
        int us=scope_at(spec->children[i],SEMANTIC_CLASS_SCOPE);
        if(us>=0)emit_anonymous_union_storage(us,top?depth:depth+1,out);
      }
      return;
    }
    if(!top)out<<indent(depth)<<"simple-declaration\n";
    unsigned d=top?depth:depth+1;
    for(std::size_t i=0;i<list->children.size();++i) {
      const Node &item=list->children[i];
      const Node *decl=item.children.empty()?0:&item.children[0];
      if(!decl)continue;
      std::string name=decl_name(*decl);
      if(name.empty())continue;
      BindingRef b=binding_at(scope,name,item.location);
      if(!b.valid())b=lookup(name,scope);
      if(!b.valid())throw std::runtime_error("declaration binding missing");
      if(b.value.kind==SEMANTIC_FUNCTION_BINDING) {
        CType t=from_type_id(b.value.type);
        if(top)out<<indent(d)<<"function-declaration "<<canonical_function_name(b)<<" "<<format_type(t)<<"\n";
        else out<<indent(d)<<"function-declaration "<<name<<" "<<format_type(t)<<"\n";
        continue;
      }
      if(b.value.kind==SEMANTIC_TYPE_ALIAS) {
        out<<indent(d)<<"type-alias "<<name<<" "<<format_type(from_type_id(b.value.type))<<"\n";
        continue;
      }
      if(b.value.kind!=SEMANTIC_VARIABLE)continue;
      CType type=from_type_id(b.value.type);
      out<<indent(d)<<"variable "<<name<<" "<<format_type(type)<<"\n";
      const Node *init=0;
      if(initializer_node(item,&init)) {
        emit_initialization(*init,scope,top?type:unqualified(type),d+1,&out);
      } else if(unqualified(type).kind==T_CLASS) {
        emit_constructor_action(type,d+1,out,name);
      }
    }
    (void)spec;
  }
  std::string function_display_name(const Node &node,int parent,int fn_scope) const {
    SemanticScopeInfo fs=model_.scope(fn_scope);
    if(fs.entity>=0) {
      SemanticEntityInfo entity=model_.entity(fs.entity);
      if(entity.name!=UINT32_MAX) {
        BindingRef ref;
        std::string name=model_.name_text(entity.name);
        SemanticNameId id=entity.name;
        std::vector<SemanticBindingInfo> named=model_.bindings_named(entity.owner_scope,id);
        for(std::size_t i=0;i<named.size();++i) {
          SemanticBindingInfo b=named[i];
          if(b.kind==SEMANTIC_FUNCTION_BINDING&&b.entity==fs.entity){ref.scope=entity.owner_scope;ref.index=0;ref.value=b;break;}
        }
        if(ref.valid())return canonical_function_name(ref);
        ref.scope=entity.owner_scope;ref.value.name=entity.name;ref.value.entity=fs.entity;
        return canonical_function_name(ref);
      }
    }
    return std::string();
  }
  int member_owner_scope(const Node &node,int fn_scope,int lexical_scope) const {
    if(lexical_scope>=0 && model_.scope(lexical_scope).kind==SEMANTIC_CLASS_SCOPE)return lexical_scope;
    SemanticScopeInfo fs=model_.scope(fn_scope);
    if(fs.entity>=0) {
      SemanticEntityInfo e=model_.entity(fs.entity);
      if(e.owner_scope>=0&&model_.scope(e.owner_scope).kind==SEMANTIC_CLASS_SCOPE)return e.owner_scope;
    }
    const Node *d=child(node,"declarator");
    if(d) {
      std::string name=decl_name(*d);std::size_t q=name.rfind("::");
      if(q!=std::string::npos) {
        std::string owner_name=name.substr(0,q);
        BindingRef b=lookup(owner_name,lexical_scope);
        if(b.valid()&&b.value.type>=0) {
          CType t=from_type_id(b.value.type);
          if(t.kind==T_CLASS)return t.owner;
        }
        for(std::size_t i=0;i<model_.scope_count();++i) {
          SemanticScopeInfo candidate=model_.scope(static_cast<int>(i));
          if(candidate.kind==SEMANTIC_CLASS_SCOPE&&qualified_scope_type_name(static_cast<int>(i),true)==owner_name)return static_cast<int>(i);
        }
      }
    }
    return -1;
  }
  CType function_type(int fn_scope) const {
    SemanticScopeInfo fs=model_.scope(fn_scope);
    if(fs.entity>=0) {
      SemanticEntityInfo e=model_.entity(fs.entity);
      return from_type_id(e.canonical_type);
    }
    return CType();
  }
  void emit_function(const Node &node,int parent,unsigned depth,std::ostream &out) {
    int fn=scope_at(node,SEMANTIC_FUNCTION_SCOPE);
    if(fn<0)throw std::runtime_error("function scope missing");
    CType type=function_type(fn);
    int owner=member_owner_scope(node,fn,parent);
    const bool is_member=owner>=0;
    type=function_with_implicit_object(type,owner);
    std::string name=function_display_name(node,parent,fn);
    if(name.empty())name=decl_name(*child(node,"declarator"));
    out<<indent(depth)<<"function-definition "<<name<<" "<<format_type(type)<<"\n";
    SemanticScopeInfo fs=model_.scope(fn);
    std::size_t parameter_index=0;
    if(is_member && type.params.size()) {
      out<<indent(depth+1)<<"parameter this "<<format_type(type.params[0])<<"\n";
      parameter_index=1;
    }
    for(std::size_t i=0;i<fs.binding_count;++i) {
      SemanticBindingInfo b=model_.binding(fn,i);
      if(b.kind==SEMANTIC_PARAMETER) {
        std::size_t type_index=parameter_index;
        if(is_member)++type_index;
        CType pt=type_index<type.params.size()?type.params[type_index]:from_type_id(b.type);
        out<<indent(depth+1)<<"parameter "<<model_.name_text(b.name)<<" "<<format_type(pt)<<"\n";
        ++parameter_index;
      }
    }
    const Node *body=0;
    for(std::size_t i=0;i<node.children.size();++i)
      if(node.children[i].text=="compound-statement"){body=&node.children[i];break;}
    if(body)statement(*body,fn,depth+1,out,type.base?*type.base:CType());
  }
  void emit_namespace(const Node &node,int parent,unsigned depth,std::ostream &out) {
    std::string name=after(node.text,"namespace-definition");name=trim(name);
    if(name.empty())name="<unnamed>";
    int ns=namespace_scope(parent,name);
    if(ns<0)throw std::runtime_error("namespace scope missing: "+name);
    out<<indent(depth)<<"namespace-definition "<<name<<"\n";
    for(std::size_t i=0;i<node.children.size();++i) {
      const Node &c=node.children[i];
      if(c.text=="inline")continue;
      top_declaration(c,ns,depth+1,out);
    }
  }
  void top_declaration(const Node &node,int scope,unsigned depth,std::ostream &out) {
    if(node.text.compare(0,std::string("namespace-definition").size(),"namespace-definition")==0){emit_namespace(node,scope,depth,out);return;}
    if(node.text=="function-definition"){emit_function(node,scope,depth,out);return;}
    if(node.text.compare(0,std::string("special-member-definition").size(),"special-member-definition")==0){emit_special_member(node,scope,depth,out);return;}
    if(node.text=="simple-declaration"){emit_simple_declaration(node,scope,depth,out,true);return;}
    if(node.text.compare(0,std::string("alias-declaration ").size(),"alias-declaration ")==0) {
      std::string name=after(node.text,"alias-declaration ");BindingRef b=lookup(name,scope);
      if(!b.valid())throw std::runtime_error("alias binding missing");
      out<<indent(depth)<<"type-alias "<<name<<" "<<format_type(from_type_id(b.value.type))<<"\n";return;
    }
    if(node.text.compare(0,std::string("linkage-specification").size(),"linkage-specification")==0 || node.text=="linkage-declaration") {
      for(std::size_t i=0;i<node.children.size();++i)top_declaration(node.children[i],scope,depth,out);
      return;
    }
    if(node.text=="template-declaration") {
      // PA7 has no template overload engine; a declaration whose body is not
      // demanded contributes no procedural output. Non-template nested
      // declarations are still visited by the PA6 semantic model.
      return;
    }
    if(node.text=="translation-unit")for(std::size_t i=0;i<node.children.size();++i)top_declaration(node.children[i],scope,depth,out);
  }
  unsigned long long converted_case_value(long long value,CType target) const {
    target=unqualified(target);
    if(target.kind==T_ENUM&&target.enum_underlying)
      target=unqualified(*target.enum_underlying);
    if(target.kind==T_BUILTIN&&unsigned_type(target)) {
      if(target.name=="unsigned int")return static_cast<unsigned int>(value);
      if(target.name=="unsigned long int")return static_cast<unsigned long>(value);
      if(target.name=="unsigned long long int")return static_cast<unsigned long long>(value);
      if(target.name=="unsigned short int")return static_cast<unsigned short>(value);
      if(target.name=="unsigned char")return static_cast<unsigned char>(value);
    }
    return static_cast<unsigned long long>(value);
  }
  ExprInfo switch_condition(const Node &node,int scope,unsigned depth,
                            std::ostream &out) {
    ExprInfo result;
    out<<indent(depth)<<"condition\n";
    if(node.children.empty())return result;
    const Node &value=node.children[0];
    if(value.text=="condition-declaration") {
      std::string name;
      for(std::size_t i=0;i<value.children.size();++i)
        if(value.children[i].text=="declarator")name=decl_name(value.children[i]);
      BindingRef binding=lookup(name,scope);
      if(!binding.valid())throw std::runtime_error("switch condition declaration binding missing");
      result.type=from_type_id(binding.value.type);
      out<<indent(depth+1)<<"condition-declaration\n";
      out<<indent(depth+2)<<"variable "<<name<<" "<<format_type(result.type)<<"\n";
      const Node *init=child(value,"initializer");
      if(init)emit_initialization(*init,scope,unqualified(result.type),depth+3,&out);
    } else result=expression(value,scope,depth+1,0,&out);
    CType type=unqualified(expression_type(result.type));
    if(!is_integral(type)&&type.kind!=T_ENUM)
      throw std::runtime_error("switch condition is not integral or enum");
    return result;
  }
  void condition(const Node &node,int scope,unsigned depth,std::ostream &out,const CType &ret) {
    out<<indent(depth)<<"condition\n";
    if(node.children.empty())return;
    const Node &c=node.children[0];
    if(c.text=="condition-declaration") {
      std::string name;
      for(std::size_t i=0;i<c.children.size();++i)if(c.children[i].text=="declarator")name=decl_name(c.children[i]);
      BindingRef b=lookup(name,scope);
      if(!b.valid())throw std::runtime_error("condition declaration binding missing");
      CType t=from_type_id(b.value.type);
      out<<indent(depth+1)<<"condition-declaration\n";
      out<<indent(depth+2)<<"variable "<<name<<" "<<format_type(t)<<"\n";
      const Node *init=child(c,"initializer");
      if(init)emit_initialization(*init,scope,unqualified(t),depth+3,&out);
      if(!is_arithmetic(t)&&t.kind!=T_POINTER&&!(t.kind==T_ENUM&&!t.scoped))throw std::runtime_error("invalid condition declaration type");
    } else {
      ExprInfo e=expression(c,scope,depth+1,0,&out);
      if(!scalar_condition(e))throw std::runtime_error("condition is not contextually convertible to bool");
    }
    (void)ret;
  }
  void statement(const Node &node,int scope,unsigned depth,std::ostream &out,const CType &return_type) {
    const std::string &text=node.text;
    if(text!="compound-statement") {
      int implicit_scope=scope_at(node,SEMANTIC_BLOCK_SCOPE,scope);
      if(implicit_scope>=0)scope=implicit_scope;
    }
    if(text=="compound-statement") {
      int block=scope_at(node,SEMANTIC_BLOCK_SCOPE,scope);
      if(block<0)block=scope_at(node,SEMANTIC_BLOCK_SCOPE);
      if(block>=0)scope=block;
      out<<indent(depth)<<"compound-statement\n";
      for(std::size_t i=0;i<node.children.size();++i)statement(node.children[i],scope,depth+1,out,return_type);
      return;
    }
    if(text=="simple-declaration") {emit_simple_declaration(node,scope,depth,out,false);return;}
    if(text.compare(0,std::string("enum-specifier").size(),"enum-specifier")==0) {
      out<<indent(depth)<<"simple-declaration\n";return;
    }
    if(text.compare(0,std::string("class-specifier").size(),"class-specifier")==0) {
      bool is_union=false;
      for(std::size_t i=0;i<node.children.size();++i)if(node.children[i].text.find("KW_UNION")!=std::string::npos)is_union=true;
      if(is_union) {
        int us=scope_at(node,SEMANTIC_CLASS_SCOPE);
        out<<indent(depth)<<"simple-declaration\n";
        if(us>=0)emit_anonymous_union_storage(us,depth+1,out);
        return;
      }
      return;
    }
    if(text=="expression-statement") {
      out<<indent(depth)<<"expression-statement\n";
      if(!node.children.empty())expression(node.children[0],scope,depth+1,0,&out);
      return;
    }
    if(text=="return-statement") {
      out<<indent(depth)<<"return-statement\n";
      if(node.children.empty()) {
        if(!is_void(return_type))throw std::runtime_error("missing return value");
      } else {
        if(is_void(return_type))throw std::runtime_error("value returned from void function");
        ExprInfo e=expression(node.children[0],scope,depth+1,&return_type,&out);
        if(!conversion(e,return_type).viable)throw std::runtime_error("invalid return conversion "+format_type(e.type)+" to "+format_type(return_type));
      }
      return;
    }
    if(text=="if-statement") {
      out<<indent(depth)<<"if-statement\n";
      if(node.children.size()>0)condition(node.children[0],scope,depth+1,out,return_type);
      for(std::size_t i=1;i<node.children.size();++i) {
        const Node &arm=node.children[i];out<<indent(depth+1)<<arm.text<<"\n";
        if(!arm.children.empty())statement(arm.children[0],scope,depth+2,out,return_type);
      }
      return;
    }
    if(text=="while-statement"||text=="switch-statement") {
      out<<indent(depth)<<text<<"\n";
      CType switch_type;
      if(!node.children.empty()) {
        if(text=="switch-statement") {
          ExprInfo sw=switch_condition(node.children[0],scope,depth+1,out);
          switch_type=unqualified(expression_type(sw.type));
          if(switch_type.kind==T_ENUM&&!switch_type.scoped)switch_type=promote(switch_type);
          else if(switch_type.kind!=T_ENUM)switch_type=promote(switch_type);
        } else condition(node.children[0],scope,depth+1,out,return_type);
      }
      if(text=="switch-statement") {
        ++switch_depth_;
        SwitchFrame frame;frame.condition_type=switch_type;
        switch_frames_.push_back(frame);
      } else ++loop_depth_;
      if(node.children.size()>1)statement(node.children[1],scope,depth+1,out,return_type);
      if(text=="switch-statement") {
        --switch_depth_;
        switch_frames_.pop_back();
      } else --loop_depth_;
      return;
    }
    if(text=="do-statement") {
      out<<indent(depth)<<"do-statement\n";
      ++loop_depth_;if(!node.children.empty())statement(node.children[0],scope,depth+1,out,return_type);--loop_depth_;
      if(node.children.size()>1)condition(node.children[1],scope,depth+1,out,return_type);
      return;
    }
    if(text=="for-statement") {
      out<<indent(depth)<<"for-statement\n";
      for(std::size_t i=0;i<node.children.size();++i) {
        const Node &c=node.children[i];
        if(c.text=="for-init-statement") {
          out<<indent(depth+1)<<"for-init-statement\n";
          if(!c.children.empty()) {
            if(c.children[0].text=="simple-declaration")statement(c.children[0],scope,depth+2,out,return_type);
            else expression(c.children[0],scope,depth+2,0,&out);
          }
        } else if(c.text=="condition")condition(c,scope,depth+1,out,return_type);
        else if(c.text=="iteration") {out<<indent(depth+1)<<"iteration\n";if(!c.children.empty())expression(c.children[0],scope,depth+2,0,&out);}
        else {++loop_depth_;statement(c,scope,depth+1,out,return_type);--loop_depth_;}
      }
      return;
    }
    if(text=="break-statement") {
      if(!loop_depth_&&!switch_depth_)throw std::runtime_error("break outside loop or switch");
      out<<indent(depth)<<"break-statement\n";return;
    }
    if(text=="continue-statement") {
      if(!loop_depth_)throw std::runtime_error("continue outside loop");
      out<<indent(depth)<<"continue-statement\n";return;
    }
    if(text=="case-statement") {
      if(!switch_depth_)throw std::runtime_error("case outside switch");
      if(node.children.size()!=2)throw std::runtime_error("malformed case");
      out<<indent(depth)<<"case-statement\n";
      ExprInfo e=expression(node.children[0],scope,depth+1,0,&out);
      if(!e.has_constant||(!is_integral(e.type)&&unqualified(e.type).kind!=T_ENUM))throw std::runtime_error("case label is not integral constant");
      if(switch_frames_.empty() ||
         !switch_frames_.back().labels.insert(converted_case_value(
             e.constant,switch_frames_.back().condition_type)).second)
        throw std::runtime_error("duplicate case label");
      statement(node.children[1],scope,depth+1,out,return_type);
      return;
    }
    if(text=="default-statement") {
      if(!switch_depth_)throw std::runtime_error("default outside switch");
      if(switch_frames_.empty() || switch_frames_.back().has_default)
        throw std::runtime_error("duplicate default label");
      switch_frames_.back().has_default=true;
      out<<indent(depth)<<"default-statement\n";
      if(!node.children.empty())statement(node.children[0],scope,depth+1,out,return_type);
      return;
    }
    if(text=="condition") {condition(node,scope,depth,out,return_type);return;}
    if(text=="namespace-definition"||text=="using-directive"||text=="using-declaration"||text.compare(0,17,"alias-declaration")==0) {
      // Block-scope aliases and using declarations affect lookup but only the
      // resulting alias/value declaration appears in this source-facing dump.
      if(text.compare(0,17,"alias-declaration")==0)top_declaration(node,scope,depth,out);
      return;
    }
    if(text=="translation-unit")return;
    if(text=="empty-declaration")return;
    if(text=="labeled-statement") {
      if(!node.children.empty())statement(node.children[0],scope,depth,out,return_type);
      return;
    }
    if(text.compare(0,14,"cast-expression")==0 || text.compare(0,std::string("call-expression").size(),"call-expression")==0 ||
       text.compare(0,15,"binary-expression")==0 || text.compare(0,19,"assignment-expression")==0) {
      out<<indent(depth)<<"expression-statement\n";expression(node,scope,depth+1,0,&out);return;
    }
    if(text=="then"||text=="else") {
      out<<indent(depth)<<text<<"\n";if(!node.children.empty())statement(node.children[0],scope,depth+1,out,return_type);return;
    }
    for(std::size_t i=0;i<node.children.size();++i)statement(node.children[i],scope,depth,out,return_type);
  }
public:
  void register_anonymous_class_nodes(const Node &node) {
    if(node.text=="class-specifier") {
      int s=scope_at(node,SEMANTIC_CLASS_SCOPE);
      if(s>=0) {
        SemanticScopeInfo info=model_.scope(s);
        std::string n=model_.name_text(info.name);
        if(info.union_class && n.find("__anonymous_class__")==0) {
          if(info.parent>=0&&model_.scope(info.parent).kind==SEMANTIC_BLOCK_SCOPE&&info.location.valid()) {
            std::ostringstream generated;
            generated<<"__anonymous_union_type__"<<(s+4)<<"_"<<(info.location.source_offset()>=6?info.location.source_offset()-6:info.location.source_offset());
            anonymous_union_names_[s]=generated.str();
          } else anonymous_union_names_[s]="__anonymous_union_type__"+n.substr(std::string("__anonymous_class__").size());
        } else if(info.union_class && n.find("__anonymous_union_type__")==0 && info.parent>=0 &&
                model_.scope(info.parent).kind==SEMANTIC_BLOCK_SCOPE && info.location.valid()) {
          std::ostringstream generated;
          generated<<"__anonymous_union_type__"<<(s+4)<<"_"<<(info.location.source_offset()>=6?info.location.source_offset()-6:info.location.source_offset());
          anonymous_union_names_[s]=generated.str();
        } else if(!info.union_class || n.find("__anonymous_union_type__")!=0)synthetic_class_name(s);
      }
    }
    for(std::size_t i=0;i<node.children.size();++i)register_anonymous_class_nodes(node.children[i]);
  }
  int class_scope_for_node(const Node &node,int parent) const {
    int s=scope_at(node,SEMANTIC_CLASS_SCOPE,parent);
    if(s>=0)return s;
    std::string name=after(node.text,"class-specifier ");
    if(!name.empty()) {
      BindingRef b=lookup(name,parent);
      if(b.valid()&&b.value.type>=0) {
        CType t=from_type_id(b.value.type);
        if(t.kind==T_CLASS)return t.owner;
      }
    }
    return -1;
  }
  void emit_special_member(const Node &node,int parent,unsigned depth,std::ostream &out) {
    int fn=scope_at(node,SEMANTIC_FUNCTION_SCOPE);
    if(fn<0)throw std::runtime_error("special member function scope missing");
    int owner=member_owner_scope(node,fn,parent);
    CType type=function_with_implicit_object(function_type(fn),owner);
    std::string name=function_display_name(node,parent,fn);
    if(name.empty())name=after(node.text,"special-member-definition ");
    out<<indent(depth)<<"function-definition "<<name<<" "<<format_type(type)<<"\n";
    bool body=false;
    for(std::size_t i=0;i<node.children.size();++i)
      if(node.children[i].text=="compound-statement") {body=true;statement(node.children[i],fn,depth+1,out,type.base?*type.base:CType());}
    if(type.params.size()&&!body) {
      out<<indent(depth+1)<<"parameter this "<<format_type(type.params[0])<<"\n";
      out<<indent(depth+1)<<"compound-statement\n";
    }
  }
  void emit_inline_class_members(const Node &node,int scope,unsigned depth,std::ostream &out) {
    if(node.text.compare(0,std::string("namespace-definition").size(),"namespace-definition")==0) {
      std::string name=trim(after(node.text,"namespace-definition"));if(name.empty())name="<unnamed>";
      int ns=namespace_scope(scope,name);if(ns>=0)
        for(std::size_t i=0;i<node.children.size();++i)emit_inline_class_members(node.children[i],ns,depth,out);
      return;
    }
    if(node.text.compare(0,std::string("function-definition").size(),"function-definition")==0)return;
    if(node.text.compare(0,std::string("special-member-definition").size(),"special-member-definition")==0)return;
    if(node.text.compare(0,std::string("class-specifier").size(),"class-specifier")==0) {
      int cls=class_scope_for_node(node,scope);if(cls<0)return;
      for(std::size_t i=0;i<node.children.size();++i) {
        const Node &c=node.children[i];
        if(c.text=="function-definition")emit_function(c,cls,depth,out);
        else if(c.text.compare(0,std::string("special-member-definition").size(),"special-member-definition")==0)emit_special_member(c,cls,depth,out);
        else emit_inline_class_members(c,cls,depth,out);
      }
      return;
    }
    for(std::size_t i=0;i<node.children.size();++i)emit_inline_class_members(node.children[i],scope,depth,out);
  }
  CallSemantics(SemanticModel &model):model_(model),root_(model.syntax_root()),global_(global_scope()),anonymous_class_serial_(0),anonymous_enum_serial_(0),loop_depth_(0),switch_depth_(0){
    canonical_type_cache_.resize(model_.type_count());
    canonical_type_cached_.resize(model_.type_count(),0);
    build_semantic_indexes();
  }
  void validate_declarations(const Node &node,std::map<int,int> &definitions) {
    if(node.text=="function-definition") {
      int fn=scope_at(node,SEMANTIC_FUNCTION_SCOPE);
      if(fn>=0) {SemanticScopeInfo f=model_.scope(fn);if(f.entity>=0&&++definitions[f.entity]>1)throw std::runtime_error("duplicate function definition");}
    }
    for(std::size_t i=0;i<node.children.size();++i)validate_declarations(node.children[i],definitions);
  }
  void validate_conflicts() const {
    for(std::size_t s=0;s<model_.scope_count();++s) {
      SemanticScopeInfo scope=model_.scope(static_cast<int>(s));
      std::unordered_map<SemanticNameId,unsigned> kinds;
      for(std::size_t i=0;i<scope.binding_count;++i) {
        SemanticBindingInfo b=model_.binding(static_cast<int>(s),i);
        unsigned bit=b.kind==SEMANTIC_FUNCTION_BINDING?1u:b.kind==SEMANTIC_VARIABLE?2u:0u;
        if(!bit)continue;
        unsigned &seen=kinds[b.name];
        if((seen|bit)==3u)throw std::runtime_error("function and variable names conflict");
        seen|=bit;
      }
    }
  }
  void run(std::ostream &out) {
    register_anonymous_class_nodes(root_);
    validate_conflicts();
    std::map<int,int> definitions;validate_declarations(root_,definitions);
    out<<"translation-unit\n";
    for(std::size_t i=0;i<root_.children.size();++i)top_declaration(root_.children[i],global_,1,out);
    for(std::size_t i=0;i<root_.children.size();++i)emit_inline_class_members(root_.children[i],global_,1,out);
    for(std::size_t i=0;i<template_instantiations_.size();++i) {
      const TemplateInstantiation &item=template_instantiations_[i];
      out<<"  function-declaration "<<item.name<<" "<<format_type(item.function)<<"\n";
      for(std::size_t j=0;j<item.function.params.size();++j)out<<"    parameter  "<<format_type(item.function.params[j])<<"\n";
    }
    emit_implicit_constructor_definitions(out);
  }
};
} // namespace

void write_call_semantics(SemanticModel &model,std::ostream &output) {
  CallSemantics(model).run(output);
}
