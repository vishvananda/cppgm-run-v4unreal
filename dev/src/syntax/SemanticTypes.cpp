#include "syntax/SemanticTypes.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace {
using Node = SyntaxNode;

enum TypeKind { TY_BUILTIN, TY_CLASS, TY_ENUM, TY_CV, TY_POINTER, TY_LREF,
                TY_RREF, TY_ARRAY, TY_FUNCTION, TY_MEMBER_POINTER };

template<class K,class V> class FlatMap {
  struct Slot {
    bool occupied;
    K key;
    V value;
    Slot() : occupied(false), key(), value() {}
  };
  std::vector<Slot> slots_;
  std::size_t count_;
  std::size_t index(const K &key) const {
    return std::hash<K>()(key) & (slots_.size()-1);
  }
  Slot *find_slot(const K &key) {
    if(slots_.empty())return 0;
    std::size_t i=index(key);
    while(slots_[i].occupied) {
      if(slots_[i].key==key)return &slots_[i];
      i=(i+1)&(slots_.size()-1);
    }
    return 0;
  }
  const Slot *find_slot(const K &key) const {
    if(slots_.empty())return 0;
    std::size_t i=index(key);
    while(slots_[i].occupied) {
      if(slots_[i].key==key)return &slots_[i];
      i=(i+1)&(slots_.size()-1);
    }
    return 0;
  }
  Slot *empty_slot(const K &key) {
    std::size_t i=index(key);
    while(slots_[i].occupied)i=(i+1)&(slots_.size()-1);
    return &slots_[i];
  }
  void rehash(std::size_t capacity) {
    std::vector<Slot> old; old.swap(slots_); slots_.resize(capacity); count_=0;
    for(std::size_t i=0;i<old.size();++i)if(old[i].occupied) {
      Slot *slot=empty_slot(old[i].key); slot->occupied=true;
      slot->key=std::move(old[i].key); slot->value=std::move(old[i].value); ++count_;
    }
  }
public:
  FlatMap() : count_(0) {}
  V *find(const K &key) { Slot *s=find_slot(key); return s?&s->value:0; }
  const V *find(const K &key) const { const Slot *s=find_slot(key); return s?&s->value:0; }
  V &operator[](const K &key) {
    Slot *found=find_slot(key); if(found)return found->value;
    if(slots_.empty())rehash(16);
    else if((count_+1)*10>=slots_.size()*7)rehash(slots_.size()*2);
    Slot *slot=empty_slot(key); slot->occupied=true; slot->key=key; ++count_;
    return slot->value;
  }
  std::size_t size() const { return count_; }
};


class FlatIdentityIndex {
  struct Slot {
    std::size_t hash;
    int value;
    bool occupied;
    Slot() : hash(0), value(-1), occupied(false) {}
  };
  std::vector<Slot> slots_;
  std::size_t count_;
  void place(std::size_t hash,int value) {
    std::size_t i=hash&(slots_.size()-1);
    while(slots_[i].occupied)i=(i+1)&(slots_.size()-1);
    slots_[i].hash=hash; slots_[i].value=value; slots_[i].occupied=true; ++count_;
  }
  void rehash(std::size_t capacity) {
    std::vector<Slot> old; old.swap(slots_); slots_.resize(capacity); count_=0;
    for(std::size_t i=0;i<old.size();++i)
      if(old[i].occupied)place(old[i].hash,old[i].value);
  }
public:
  FlatIdentityIndex() : count_(0) {}
  template<class Equal> int find(std::size_t hash,const Equal &equal) const {
    if(slots_.empty())return -1;
    std::size_t i=hash&(slots_.size()-1);
    while(slots_[i].occupied) {
      if(slots_[i].hash==hash && equal(slots_[i].value))return slots_[i].value;
      i=(i+1)&(slots_.size()-1);
    }
    return -1;
  }
  void insert(std::size_t hash,int value) {
    if(slots_.empty())rehash(16);
    else if((count_+1)*10>=slots_.size()*7)rehash(slots_.size()*2);
    place(hash,value);
  }
};

struct BindingIndices {
  int first;
  std::vector<int> later;
  BindingIndices() : first(-1) {}
  void push_back(int id) { if(first<0)first=id; else later.push_back(id); }
  bool empty() const { return first<0; }
  std::size_t size() const { return first<0?0:1+later.size(); }
  int back() const { return later.empty()?first:later.back(); }
  int operator[](std::size_t i) const { return i==0?first:later[i-1]; }
};
class NameInterner {
  std::vector<std::string> names_;
  std::vector<std::uint32_t> slots_; // ID + 1; zero denotes an empty slot.
  std::size_t index(const std::string &s) const { return std::hash<std::string>()(s)&(slots_.size()-1); }
  void rehash(std::size_t cap) {
    std::vector<std::uint32_t> old; old.swap(slots_); slots_.assign(cap,0);
    for(std::size_t i=0;i<names_.size();++i) {
      std::size_t p=index(names_[i]); while(slots_[p])p=(p+1)&(slots_.size()-1);
      slots_[p]=static_cast<std::uint32_t>(i+1);
    }
  }
public:
  std::uint32_t intern(const std::string &s) {
    if(slots_.empty())rehash(16);
    std::size_t p=index(s);
    while(slots_[p]) {
      const std::uint32_t id=slots_[p]-1;
      if(names_[id]==s)return id;
      p=(p+1)&(slots_.size()-1);
    }
    const std::uint32_t id=static_cast<std::uint32_t>(names_.size());
    names_.push_back(s); slots_[p]=id+1;
    if(names_.size()*10>=slots_.size()*7)rehash(slots_.size()*2);
    return id;
  }
  int find(const std::string &s) const {
    if(slots_.empty())return -1;
    std::size_t p=index(s);
    while(slots_[p]) {
      const std::uint32_t id=slots_[p]-1;
      if(names_[id]==s)return static_cast<int>(id);
      p=(p+1)&(slots_.size()-1);
    }
    return -1;
  }
  const std::string &text(std::uint32_t id) const { return names_[id]; }
};

class SmallIntList {
  static const std::size_t inline_capacity=4;
  std::size_t size_;
  int inline_[inline_capacity];
  std::vector<int> overflow_;
public:
  SmallIntList() : size_(0) {}
  SmallIntList &operator=(const std::vector<int> &values) {
    size_=0; overflow_.clear();
    for(std::size_t i=0;i<values.size();++i)push_back(values[i]);
    return *this;
  }
  void push_back(int value) {
    if(size_<inline_capacity)inline_[size_]=value;
    else overflow_.push_back(value);
    ++size_;
  }
  std::size_t size() const { return size_; }
  bool empty() const { return size_==0; }
  int operator[](std::size_t i) const {
    return i<inline_capacity?inline_[i]:overflow_[i-inline_capacity];
  }
  friend bool operator==(const SmallIntList &a,const SmallIntList &b) {
    if(a.size_!=b.size_)return false;
    for(std::size_t i=0;i<a.size_;++i)if(a[i]!=b[i])return false;
    return true;
  }
};

struct Type {
  TypeKind kind;
  int base;
  SmallIntList params;
  unsigned long long bound;
  bool variadic, is_const, is_volatile, scoped;
  int owner_scope;
  int function_signature;
  int function_identity;
  std::string tag;
  std::uint32_t name_id;
  bool template_template;
  Type() : kind(TY_BUILTIN), base(-1), bound(0), variadic(false),
           is_const(false), is_volatile(false), scoped(false), owner_scope(-1), function_signature(-1),
           function_identity(-1), name_id(UINT32_MAX), template_template(false) {}
};
enum DisplayTypeKind { DISPLAY_DEFAULT, DISPLAY_CLASS, DISPLAY_STRUCT,
                        DISPLAY_UNION, DISPLAY_ENUM, DISPLAY_ENUM_CLASS };
struct Binding {
  std::uint32_t name_id;
  std::string kind;
  DisplayTypeKind display_kind;
  std::uint32_t display_name_id;
  int type;
  int target_scope;
  long long value;
  bool has_value;
  int enum_id;
  int function_identity;
  SyntaxLocation location;
  int entity_id;
  bool is_static;
  Binding() : name_id(0), display_kind(DISPLAY_DEFAULT), display_name_id(UINT32_MAX),
              type(-1), target_scope(-1), value(0), has_value(false), enum_id(0),
              function_identity(-1), entity_id(-1), is_static(false) {}
};
struct Scope {
  std::string kind;
  std::uint32_t name_id;
  int parent;
  int entity_id;
  SyntaxLocation location;
  bool visible, inline_namespace;
  bool union_class, layout_computed, layout_in_progress, layout_unsupported, layout_complete, has_virtual, layout_empty, layout_has_vptr;
  long long object_size, object_data_size, object_alignment;
  std::vector<int> base_types;
  std::vector<Binding> bindings;
  FlatMap<std::uint32_t,BindingIndices> by_name;
  FlatMap<std::uint64_t,int> child_index;
  std::vector<int> children, directives;
  Scope() : name_id(UINT32_MAX), parent(-1), entity_id(-1), visible(true), inline_namespace(false),
            union_class(false), layout_computed(false), layout_in_progress(false),
            layout_unsupported(false), layout_complete(false), has_virtual(false),
            layout_empty(false), layout_has_vptr(false), object_size(0), object_data_size(0), object_alignment(1) {}
};
struct Entity {
  std::string kind;
  int owner_scope;
  std::uint32_t name_id;
  int canonical_type;
  int function_signature;
  SyntaxLocation first_location;
  Entity() : owner_scope(-1), name_id(0), canonical_type(-1), function_signature(-1) {}
};
struct FunctionSignature {
  int return_type;
  SmallIntList parameters;
  bool variadic;
  FunctionSignature() : return_type(-1), variadic(false) {}
};
struct FunctionIdentity {
  SmallIntList parameters;
  bool variadic;
  bool is_const, is_volatile;
  FunctionIdentity() : variadic(false), is_const(false), is_volatile(false) {}
};
struct ConstValue {
  long long value;
  int enum_id;
  bool valid;
  ConstValue() : value(0), enum_id(0), valid(false) {}
  ConstValue(long long v, int e = 0) : value(v), enum_id(e), valid(true) {}
};

std::string trim(const std::string &s) {
  std::size_t a = 0, b = s.size();
  while (a < b && s[a] == ' ') ++a;
  while (b > a && s[b-1] == ' ') --b;
  return s.substr(a, b-a);
}
std::string after_prefix(const std::string &s, const std::string &p) {
  return s.compare(0, p.size(), p) == 0 ? s.substr(p.size()) : std::string();
}
std::string tail_name(const std::string &s) {
  std::size_t p = s.rfind("::");
  return p == std::string::npos ? s : s.substr(p + 2);
}
std::vector<std::string> split_qualified(const std::string &s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size()) {
    if (s.compare(i, 2, "::") == 0) { i += 2; continue; }
    std::size_t j = s.find("::", i);
    if (j == std::string::npos) j = s.size();
    out.push_back(s.substr(i, j-i));
    i = j;
  }
  return out;
}

class Analyzer {
  SyntaxTree &tree_;
  std::vector<Type> types_;
  NameInterner names_;
  FlatMap<std::string,int> builtin_;
  FlatIdentityIndex type_index_;
  std::vector<FunctionSignature> function_signatures_;
  FlatIdentityIndex function_signature_index_;
  std::vector<FunctionIdentity> function_identities_;
  FlatIdentityIndex function_identity_index_;
  std::vector<Entity> entities_;
  std::vector<Scope> scopes_;
  std::vector<PostTokenSourceFile> source_files_;
  mutable std::vector<std::vector<std::size_t> > source_line_starts_;
  mutable std::vector<std::uint64_t> lookup_marks_;
  mutable std::uint64_t lookup_generation_;
  int global_;
  bool allow_anon_union_;
  unsigned anonymous_union_serial_;
  bool retain_syntax_;

  static void hash_combine(std::size_t &seed,std::size_t value) {
    seed ^= value + static_cast<std::size_t>(0x9e3779b9U) + (seed << 6) + (seed >> 2);
  }
  static bool same_type(const Type &a,const Type &b) {
    return a.kind==b.kind && a.base==b.base && a.params==b.params &&
           a.bound==b.bound && a.variadic==b.variadic &&
           a.is_const==b.is_const && a.is_volatile==b.is_volatile &&
           a.scoped==b.scoped && a.owner_scope==b.owner_scope &&
           a.name_id==b.name_id && a.template_template==b.template_template;
  }
  static std::size_t type_hash(const Type &t) {
    std::size_t h=static_cast<std::size_t>(t.kind);
    hash_combine(h,static_cast<std::size_t>(t.base+1));
    hash_combine(h,static_cast<std::size_t>(t.bound));
    hash_combine(h,static_cast<std::size_t>(t.variadic));
    hash_combine(h,static_cast<std::size_t>(t.is_const));
    hash_combine(h,static_cast<std::size_t>(t.is_volatile));
    hash_combine(h,static_cast<std::size_t>(t.scoped));
    hash_combine(h,static_cast<std::size_t>(t.owner_scope+1));
    hash_combine(h,static_cast<std::size_t>(t.name_id));
    hash_combine(h,static_cast<std::size_t>(t.template_template));
    for(std::size_t i=0;i<t.params.size();++i)
      hash_combine(h,static_cast<std::size_t>(t.params[i]+1));
    return h;
  }
  int new_type(const Type &t) {
    const std::size_t hash=type_hash(t);
    const int found=type_index_.find(hash,[&](int id) {
      return same_type(types_[id],t);
    });
    if(found>=0)return found;
    const int id=static_cast<int>(types_.size());
    types_.push_back(t);
    type_index_.insert(hash,id);
    return id;
  }
  int builtin(const std::string &name) {
    if(const int *f=builtin_.find(name))return *f;
    Type t; t.kind=TY_BUILTIN; t.name_id=intern_name(name);
    int id=new_type(t); builtin_[name]=id; return id;
  }
  bool is_void_type(int id) const {
    while(id>=0 && types_[id].kind==TY_CV)id=types_[id].base;
    return id>=0 && types_[id].kind==TY_BUILTIN && name_text(types_[id].name_id)=="void";
  }
  int cv_type(int id, bool c, bool v) {
    if (!c && !v) return id;
    if (id < 0) return id;
    const Type &old=types_[id];
    if (old.kind==TY_LREF || old.kind==TY_RREF) return id;
    if (old.kind==TY_ARRAY) return array_type(cv_type(old.base,c,v),old.bound);
    Type t;
    if (old.kind==TY_CV) {
      t=old; t.is_const=t.is_const||c; t.is_volatile=t.is_volatile||v;
    } else { t.kind=TY_CV; t.base=id; t.is_const=c; t.is_volatile=v; }
    return new_type(t);
  }
  int pointer_type(int id, bool c=false, bool v=false) {
    if (id<0 || types_[id].kind==TY_LREF || types_[id].kind==TY_RREF)
      throw std::runtime_error("pointer to reference type");
    Type t; t.kind=TY_POINTER; t.base=id;
    int out=new_type(t); return cv_type(out,c,v);
  }
  int reference_type(int id, bool rvalue) {
    if (id<0) throw std::runtime_error("invalid reference type");
    if(is_void_type(id))throw std::runtime_error("reference to void");
    if (types_[id].kind==TY_LREF || types_[id].kind==TY_RREF) {
      bool lvalue = !rvalue || types_[id].kind==TY_LREF;
      Type t; t.kind=lvalue ? TY_LREF : TY_RREF; t.base=types_[id].base;
      return new_type(t);
    }
    Type t; t.kind=rvalue ? TY_RREF : TY_LREF; t.base=id; return new_type(t);
  }
  int array_type(int id, unsigned long long n) {
    if (id<0 || types_[id].kind==TY_LREF || types_[id].kind==TY_RREF ||
        types_[id].kind==TY_FUNCTION || is_void_type(id))
      throw std::runtime_error("invalid array element type");
    Type t; t.kind=TY_ARRAY; t.base=id; t.bound=n; return new_type(t);
  }
  int adjusted_parameter(int id) {
    if(id<0)return id;
    while(types_[id].kind==TY_CV)id=types_[id].base;
    if(types_[id].kind==TY_ARRAY)return pointer_type(types_[id].base);
    if(types_[id].kind==TY_FUNCTION)return pointer_type(id);
    return id;
  }
  SmallIntList adjusted_parameters(const std::vector<int> &source_params) {
    SmallIntList adjusted;
    for(std::size_t i=0;i<source_params.size();++i)
      adjusted.push_back(adjusted_parameter(source_params[i]));
    return adjusted;
  }
  int intern_function_identity(const std::vector<int> &source_params,bool variadic,
                               bool is_const=false,bool is_volatile=false) {
    FunctionIdentity identity; identity.variadic=variadic;
    identity.is_const=is_const; identity.is_volatile=is_volatile;
    identity.parameters=adjusted_parameters(source_params);
    std::size_t hash=static_cast<std::size_t>(variadic);
    hash_combine(hash,static_cast<std::size_t>(is_const));
    hash_combine(hash,static_cast<std::size_t>(is_volatile));
    for(std::size_t i=0;i<identity.parameters.size();++i)
      hash_combine(hash,static_cast<std::size_t>(identity.parameters[i]+1));
    const int found=function_identity_index_.find(hash,[&](int id) {
      const FunctionIdentity &old=function_identities_[id];
      return old.variadic==variadic && old.is_const==is_const &&
             old.is_volatile==is_volatile && old.parameters==identity.parameters;
    });
    if(found>=0)return found;
    const int id=static_cast<int>(function_identities_.size());
    function_identities_.push_back(identity);
    function_identity_index_.insert(hash,id);
    return id;
  }
  int intern_function_signature(int ret,const std::vector<int> &source_params,bool variadic) {
    FunctionSignature signature; signature.return_type=ret; signature.variadic=variadic;
    signature.parameters=adjusted_parameters(source_params);
    std::size_t hash=static_cast<std::size_t>(variadic);
    hash_combine(hash,static_cast<std::size_t>(ret+1));
    for(std::size_t i=0;i<signature.parameters.size();++i)
      hash_combine(hash,static_cast<std::size_t>(signature.parameters[i]+1));
    const int found=function_signature_index_.find(hash,[&](int id) {
      const FunctionSignature &old=function_signatures_[id];
      return old.return_type==ret && old.variadic==variadic &&
             old.parameters==signature.parameters;
    });
    if(found>=0)return found;
    const int id=static_cast<int>(function_signatures_.size());
    function_signatures_.push_back(signature);
    function_signature_index_.insert(hash,id);
    return id;
  }
  int function_type(int ret,const std::vector<int> &params,bool variadic,
                    bool is_const=false,bool is_volatile=false) {
    if (ret<0) throw std::runtime_error("invalid function return type");
    Type t; t.kind=TY_FUNCTION; t.base=ret; t.params=params; t.variadic=variadic;
    t.is_const=is_const; t.is_volatile=is_volatile;
    t.function_signature=intern_function_signature(ret,params,variadic);
    t.function_identity=intern_function_identity(params,variadic,is_const,is_volatile);
    return new_type(t);
  }
  int member_pointer_type(int base,int owner_scope) {
    if(base<0||owner_scope<0)throw std::runtime_error("invalid member pointer type");
    Type t;t.kind=TY_MEMBER_POINTER;t.base=base;t.owner_scope=owner_scope;
    return new_type(t);
  }
  int class_type(int owner,const std::string &tag,const std::string &name) {
    Type t; t.kind=TY_CLASS; t.owner_scope=owner; t.tag=tag;
    t.name_id=intern_name(name);
    // new_type computes a structural key separately from the display tag.
    return new_type(t);
  }
  int enum_type(int owner,const std::string &name,bool scoped) {
    Type t; t.kind=TY_ENUM; t.owner_scope=owner; t.scoped=scoped;
    t.name_id=intern_name(name);
    return new_type(t);
  }
  static std::uint32_t scope_kind_code(const std::string &kind) {
    if(kind=="namespace")return 1;
    if(kind=="class")return 2;
    if(kind=="enum")return 3;
    if(kind=="function")return 4;
    if(kind=="template-parameters")return 5;
    if(kind=="block")return 6;
    return 0;
  }
  static std::uint64_t scope_child_key(std::uint32_t kind,std::uint32_t name) {
    return (static_cast<std::uint64_t>(kind)<<32)|name;
  }
  int add_scope(const std::string &kind,const std::string &name,int parent,
                bool visible=true,const SyntaxLocation &location=SyntaxLocation()) {
    Scope s; s.kind=kind; s.parent=parent; s.visible=visible;
    s.location=location;
    const std::uint32_t name_id=intern_name(name);
    s.name_id=name_id;
    int id=static_cast<int>(scopes_.size()); scopes_.push_back(s);
    if (parent>=0) {
      scopes_[parent].children.push_back(id);
      scopes_[parent].child_index[scope_child_key(scope_kind_code(kind),name_id)]=id;
    }
    return id;
  }
  std::uint32_t intern_name(const std::string &name) { return names_.intern(name); }
  int find_name_id(const std::string &name) const { return names_.find(name); }
  const std::string &name_text(std::uint32_t id) const { return names_.text(id); }
  int add_entity(int scope,std::uint32_t name_id,const std::string &kind,
                 int type,int signature=-1,
                 const SyntaxLocation &location=SyntaxLocation()) {
    Entity e; e.owner_scope=scope; e.name_id=name_id; e.kind=kind;
    e.canonical_type=type; e.function_signature=signature; e.first_location=location;
    const int id=static_cast<int>(entities_.size()); entities_.push_back(e); return id;
  }
  int add_binding(int scope,const std::string &name,const std::string &kind,
                  int type=-1,int target=-1,int existing_entity=-1,
                  const SyntaxLocation &location=SyntaxLocation()) {
    if (scope<0 || scope>=static_cast<int>(scopes_.size()))
      throw std::runtime_error("invalid semantic scope");
    Binding b; b.name_id=intern_name(name); b.kind=kind; b.type=type; b.target_scope=target;
    b.location=location;
    if(kind=="function" && type>=0 && types_[type].kind==TY_FUNCTION)
      b.function_identity=types_[type].function_identity;
    const BindingIndices *prior=scopes_[scope].by_name.find(b.name_id);
    if(prior) for(std::size_t i=0;i<prior->size();++i) {
      const Binding &old=scopes_[scope].bindings[(*prior)[i]];
      const bool old_ns=old.kind=="namespace" || old.kind=="namespace-alias";
      const bool new_ns=kind=="namespace" || kind=="namespace-alias";
      if(old_ns!=new_ns)throw std::runtime_error("namespace conflicts with ordinary name");
    }
    if(existing_entity>=0)b.entity_id=existing_entity;
    else if((kind=="namespace" || kind=="namespace-alias") && target>=0 &&
            scopes_[target].entity_id>=0)b.entity_id=scopes_[target].entity_id;
    else b.entity_id=add_entity(scope,b.name_id,kind,type,
                                kind=="function" && type>=0 && types_[type].kind==TY_FUNCTION?
                                types_[type].function_signature:b.function_identity,location);
    int id=static_cast<int>(scopes_[scope].bindings.size());
    scopes_[scope].bindings.push_back(b); scopes_[scope].by_name[b.name_id].push_back(id);
    return id;
  }
  int bind_value(int scope,const std::string &name,const std::string &kind,
                 int type,const SyntaxLocation &location=SyntaxLocation()) {
    return add_binding(scope,name,kind,type,-1,-1,location);
  }
  const Node *find_child(const Node &n,const std::string &label) const {
    for (std::size_t i=0;i<n.children.size();++i)
      if (n.children[i].text==label) return &n.children[i];
    return 0;
  }
  std::string declarator_name(const Node &n) const {
    if (n.text.compare(0,11,"identifier ")==0) return n.text.substr(11);
    for (std::size_t i=0;i<n.children.size();++i) {
      std::string s=declarator_name(n.children[i]); if (!s.empty()) return s;
    }
    return std::string();
  }
  std::string specifier_value(const Node &n) const {
    std::string s=after_prefix(n.text,"decl-specifier ");
    std::size_t colon=s.find(':');
    if (colon!=std::string::npos && (s.compare(0,3,"KW_")==0 || s.compare(0,13,"TT_IDENTIFIER")==0))
      return s.substr(colon+1);
    if (s.compare(0,13,"TT_IDENTIFIER")==0) return s.substr(14);
    return s;
  }
  std::string class_key(const Node &n) const {
    for (std::size_t i=0;i<n.children.size();++i) {
      if (n.children[i].text.compare(0,10,"class-key ")==0) {
        std::string v=n.children[i].text.substr(10);
        std::size_t p=v.find(':'); return p==std::string::npos?v:v.substr(p+1);
      }
    }
    return "class";
  }
  std::string node_decl_name(const Node &n,const std::string &prefix) const {
    std::string s=after_prefix(n.text,prefix);
    return trim(s);
  }
  bool has_spec(const Node &spec,const std::string &name) const {
    for (std::size_t i=0;i<spec.children.size();++i)
      if (specifier_value(spec.children[i])==name) return true;
    return false;
  }
  int root_scope(int scope) const {
    int s=scope;
    while (s>=0 && scopes_[s].parent>=0) s=scopes_[s].parent;
    return s;
  }
  int latest_binding(int scope,SemanticNameId name_id) const {
    if(scope<0)return -1;
    const BindingIndices *f=scopes_[scope].by_name.find(name_id);
    if(!f || f->empty())return -1;
    return f->back();
  }
  int latest_binding(int scope,const std::string &name) const {
    const int id=find_name_id(name);
    return id<0?-1:latest_binding(scope,static_cast<SemanticNameId>(id));
  }
  const Binding &binding(int scope,int index) const { return scopes_[scope].bindings[index]; }
  int binding_type(int scope,int index) const { return binding(scope,index).type; }
  int namespace_target(int scope,int index) const {
    const Binding &b=binding(scope,index);
    if (b.kind=="namespace" || b.kind=="namespace-alias") return b.target_scope;
    return -1;
  }
  void add_namespace_binding(int scope,const std::string &name,int child,
                             const std::string &kind="namespace",
                             const SyntaxLocation &location=SyntaxLocation()) {
    int prev=latest_binding(scope,name);
    if (prev>=0) {
      const Binding &old=binding(scope,prev);
      if (old.kind=="namespace" && kind=="namespace") return;
      if (old.kind=="namespace-alias" && kind=="namespace-alias" &&
          old.target_scope==child) return;
      throw std::runtime_error("namespace conflicts with existing binding");
    }
    add_binding(scope,name,kind,-1,child,-1,location);
  }
  int find_child_scope(int scope,const std::string &kind,const std::string &name) const {
    const int name_id=find_name_id(name);
    if(scope<0 || name_id<0)return -1;
    const int *found=scopes_[scope].child_index.find(
        scope_child_key(scope_kind_code(kind),static_cast<std::uint32_t>(name_id)));
    return found?*found:-1;
  }
  std::pair<int,int> lookup_direct_or_directive(int scope,SemanticNameId name,
                                                   bool type_only=false,
                                                   bool namespace_only=false) const {
    if(scope<0 || scope>=static_cast<int>(scopes_.size()))return std::make_pair(-1,-1);
    const int direct=latest_binding(scope,name);
    if(direct>=0) {
      const Binding &b=binding(scope,direct);
      const bool is_type=b.kind=="type" || b.kind=="type-alias";
      const bool is_ns=b.kind=="namespace" || b.kind=="namespace-alias";
      if((!type_only || is_type) && (!namespace_only || is_ns))return std::make_pair(scope,direct);
      // A declaration of this spelling hides outer ordinary-name bindings;
      // category-filtered lookup must not fall through to a parent or using edge.
      if(!namespace_only)return std::make_pair(-1,-1);
      // Namespace-target lookup uses the namespace-name namespace and may
      // continue past an ordinary value with the same spelling.
    }
    std::vector<int> work;
    if(lookup_marks_.size()<scopes_.size())lookup_marks_.resize(scopes_.size(),0);
    if(++lookup_generation_==0) {
      std::fill(lookup_marks_.begin(),lookup_marks_.end(),0);
      lookup_generation_=1;
    }
    const std::uint64_t epoch=lookup_generation_;
    lookup_marks_[scope]=epoch;
    const Scope &root=scopes_[scope];
    for(std::size_t i=0;i<root.directives.size();++i)work.push_back(root.directives[i]);
    for(std::size_t i=0;i<root.children.size();++i) {
      const int child=root.children[i];
      if(scopes_[child].kind=="namespace" &&
         (scopes_[child].inline_namespace || name_text(scopes_[child].name_id)=="<unnamed>"))work.push_back(child);
    }
    std::pair<int,int> found(-1,-1);
    for(std::size_t pos=0;pos<work.size();++pos) {
      const int current=work[pos];
      if(current<0 || current>=static_cast<int>(scopes_.size()) || lookup_marks_[current]==epoch)continue;
      lookup_marks_[current]=epoch;
      const int candidate=latest_binding(current,name);
      if(candidate>=0) {
        const Binding &b=binding(current,candidate);
        const bool is_type=b.kind=="type" || b.kind=="type-alias";
        const bool is_ns=b.kind=="namespace" || b.kind=="namespace-alias";
        if((!type_only || is_type) && (!namespace_only || is_ns)) {
          if(found.first>=0 && (found.first!=current || found.second!=candidate))
            throw std::runtime_error("ambiguous using-directive lookup");
          found=std::make_pair(current,candidate);
        }
      }
      for(std::size_t i=0;i<scopes_[current].directives.size();++i)
        work.push_back(scopes_[current].directives[i]);
      for(std::size_t i=0;i<scopes_[current].children.size();++i) {
        const int child=scopes_[current].children[i];
        if(scopes_[child].kind=="namespace" &&
           (scopes_[child].inline_namespace || name_text(scopes_[child].name_id)=="<unnamed>"))work.push_back(child);
      }
    }
    return found;
  }
  std::pair<int,int> lookup_direct_or_directive(int scope,const std::string &name,
                                                   bool type_only=false,
                                                   bool namespace_only=false) const {
    const int id=find_name_id(name);
    return id<0?std::make_pair(-1,-1):
        lookup_direct_or_directive(scope,static_cast<SemanticNameId>(id),type_only,namespace_only);
  }
  std::pair<int,int> unqualified_lookup(int scope,SemanticNameId name,
                                        bool type_only=false,bool namespace_only=false) const {
    int here=scope;
    while(here>=0) {
      std::pair<int,int> result=lookup_direct_or_directive(here,name,type_only,namespace_only);
      if(result.first>=0)return result;
      if(type_only && !namespace_only && latest_binding(here,name)>=0)
        return std::make_pair(-1,-1);
      here=scopes_[here].parent;
    }
    return std::make_pair(-1,-1);
  }
  std::pair<int,int> unqualified_lookup(int scope,const std::string &name,
                                        bool type_only=false,bool namespace_only=false) const {
    const int id=find_name_id(name);
    return id<0?std::make_pair(-1,-1):
        unqualified_lookup(scope,static_cast<SemanticNameId>(id),type_only,namespace_only);
  }
  std::pair<int,int> qualified_scope_component(int scope,SemanticNameId name) const {
    bool hidden_ordinary=false;
    // Search the complete lexical chain for a direct namespace/type binding
    // before consulting directives at any nearer scope.
    for(int here=scope;here>=0;here=scopes_[here].parent) {
      const int direct=latest_binding(here,name);
      if(direct<0)continue;
      const Binding &b=binding(here,direct);
      if(b.kind=="namespace" || b.kind=="namespace-alias")return std::make_pair(here,direct);
      if(b.kind=="type" || b.kind=="type-alias") {
        int type=b.type;
        while(type>=0 && types_[type].kind==TY_CV)type=types_[type].base;
        if(type>=0 && (types_[type].kind==TY_CLASS || types_[type].kind==TY_ENUM))return std::make_pair(here,direct);
        return std::make_pair(-1,-1);
      }
      hidden_ordinary=true;
    }
    for(int here=scope;here>=0;here=scopes_[here].parent) {
      const std::pair<int,int> ns=lookup_direct_or_directive(here,name,false,true);
      if(ns.first>=0)return ns;
    }
    if(hidden_ordinary)return std::make_pair(-1,-1);
    for(int here=scope;here>=0;here=scopes_[here].parent) {
      const std::pair<int,int> type=lookup_direct_or_directive(here,name,true,false);
      if(type.first>=0) {
        int id=binding_type(type.first,type.second);
        while(id>=0 && types_[id].kind==TY_CV)id=types_[id].base;
        if(id>=0 && (types_[id].kind==TY_CLASS || types_[id].kind==TY_ENUM))return type;
      }
    }
    return std::make_pair(-1,-1);
  }
  std::pair<int,int> qualified_scope_component(int scope,const std::string &name) const {
    const int id=find_name_id(name);
    return id<0?std::make_pair(-1,-1):
        qualified_scope_component(scope,static_cast<SemanticNameId>(id));
  }
  std::pair<int,int> qualified_lookup_ids(int scope,
                                          const std::vector<SemanticNameId> &parts,
                                          bool absolute,bool type_only,
                                          bool namespace_only) const {
    if(parts.empty())return std::make_pair(-1,-1);
    int current=absolute?root_scope(scope):scope;
    for(std::size_t i=0;i+1<parts.size();++i) {
      const std::pair<int,int> b=qualified_scope_component(current,parts[i]);
      if(b.first<0)return std::make_pair(-1,-1);
      int target=namespace_target(b.first,b.second);
      if(target<0) {
        int type=binding_type(b.first,b.second);
        if(type<0)return std::make_pair(-1,-1);
        while(types_[type].kind==TY_CV)type=types_[type].base;
        if(types_[type].kind!=TY_CLASS && types_[type].kind!=TY_ENUM)
          return std::make_pair(-1,-1);
        target=types_[type].owner_scope;
        if(types_[type].kind==TY_ENUM) {
          const int *definition=enum_definition_lookup_.find(type);
          if(definition)target=*definition;
        }
      }
      current=target;
    }
    return lookup_direct_or_directive(current,parts.back(),type_only,namespace_only);
  }
  std::pair<int,int> qualified_lookup(int scope,const std::string &full,
                                      bool type_only=false,bool namespace_only=false) const {
    std::vector<std::string> parts=split_qualified(full);
    if (parts.empty()) return std::make_pair(-1,-1);
    int current=scope;
    if (full.compare(0,2,"::")==0) current=root_scope(scope);
    for (std::size_t i=0;i+1<parts.size();++i) {
      std::pair<int,int> b=qualified_scope_component(current,parts[i]);
      if (b.first<0) return std::make_pair(-1,-1);
      int target=namespace_target(b.first,b.second);
      if (target<0) {
        int ty=binding_type(b.first,b.second);
        if (ty<0) return std::make_pair(-1,-1);
        while (types_[ty].kind==TY_CV) ty=types_[ty].base;
        if (types_[ty].kind!=TY_CLASS && types_[ty].kind!=TY_ENUM) return std::make_pair(-1,-1);
        target=types_[ty].owner_scope;
        if(types_[ty].kind==TY_ENUM) {
          const int *def=enum_definition_lookup_.find(ty);
          if(def)target=*def;
        }
      }
      current=target;
    }
    return lookup_direct_or_directive(current,parts.back(),type_only,namespace_only);
  }
  std::pair<int,int> resolve_name(int scope,const std::string &name,
                                  bool type_only=false,bool namespace_only=false) const {
    if (name.find("::")!=std::string::npos) return qualified_lookup(scope,name,type_only,namespace_only);
    return unqualified_lookup(scope,name,type_only,namespace_only);
  }
  int require_type(int scope,const std::string &name) const {
    std::pair<int,int> found=resolve_name(scope,name,true,false);
    if (found.first<0) throw std::runtime_error("unknown type name: "+name);
    int ty=binding_type(found.first,found.second);
    if (ty<0) throw std::runtime_error("name does not denote a type");
    return ty;
  }
  int require_namespace(int scope,const std::string &name) const {
    std::pair<int,int> found=resolve_name(scope,name,false,true);
    if (found.first<0) throw std::runtime_error("unknown namespace: "+name);
    return namespace_target(found.first,found.second);
  }
  std::string type_name(int id) const {
    if (id<0) return "<invalid>";
    const Type &t=types_[id];
    switch (t.kind) {
    case TY_BUILTIN: return name_text(t.name_id);
    case TY_CLASS: return (t.tag.empty()?"class":t.tag)+" "+name_text(t.name_id);
    case TY_ENUM: return std::string(t.scoped?"enum class ":"enum ")+name_text(t.name_id);
    case TY_CV: {
      std::string p;
      if (t.is_const) p="const";
      if (t.is_volatile) p+=(p.empty()?"":" ")+std::string("volatile");
      return p+" "+type_name(t.base);
    }
    case TY_POINTER: return "pointer to "+type_name(t.base);
    case TY_LREF: return "lvalue-reference to "+type_name(t.base);
    case TY_RREF: return "rvalue-reference to "+type_name(t.base);
    case TY_ARRAY: return "array of "+(t.bound?number(t.bound):std::string("0"))+" "+type_name(t.base);
    case TY_FUNCTION: {
      std::string r="function of (";
      for (std::size_t i=0;i<t.params.size();++i) {
        if (i) r+=", ";
        r+=type_name(t.params[i]);
      }
      if (t.variadic) { if (!t.params.empty()) r+=", "; r+="..."; }
      r+=")";
      if(t.is_const)r+=" const";
      if(t.is_volatile)r+=" volatile";
      return r+" returning "+type_name(t.base);
    }
    case TY_MEMBER_POINTER: {
      std::string owner="<invalid class>";
      if(t.owner_scope>=0 && scopes_[t.owner_scope].entity_id>=0) {
        const int type=entities_[scopes_[t.owner_scope].entity_id].canonical_type;
        if(type>=0)owner=type_name(type);
      }
      return "member-pointer of "+owner+" to "+type_name(t.base);
    }
    }
    return "<invalid>";
  }
  static std::string number(unsigned long long v) {
    std::ostringstream out; out<<v; return out.str();
  }
  std::string binding_display_type(const Binding &b) const {
    const std::uint32_t name_id=b.display_name_id==UINT32_MAX?b.name_id:b.display_name_id;
    switch(b.display_kind) {
    case DISPLAY_CLASS:return "class "+name_text(name_id);
    case DISPLAY_STRUCT:return "struct "+name_text(name_id);
    case DISPLAY_UNION:return "union "+name_text(name_id);
    case DISPLAY_ENUM:return "enum "+name_text(name_id);
    case DISPLAY_ENUM_CLASS:return "enum class "+name_text(name_id);
    case DISPLAY_DEFAULT:return type_name(b.type);
    }
    return type_name(b.type);
  }
  std::string output_binding(const Binding &b) const {
    if (b.kind=="namespace" || b.kind=="namespace-alias") return std::string();
    if (b.kind=="type" || b.kind=="type-alias")
      return b.kind+" "+name_text(b.name_id)+" "+binding_display_type(b);
    if (b.kind=="enumerator") {
      std::ostringstream out; out<<"enumerator "<<name_text(b.name_id)<<" "<<binding_display_type(b)<<" "<<b.value;
      return out.str();
    }
    return b.kind+" "+name_text(b.name_id)+" "+binding_display_type(b);
  }
  int fundamental_type(const std::vector<std::string> &tokens,int scope) {
    bool uns=false, sign=false; int longs=0; bool sh=false, ischar=false;
    bool isbool=false, isvoid=false, isfloat=false, isdouble=false;
    std::string other;
    for (std::size_t i=0;i<tokens.size();++i) {
      const std::string &s=tokens[i];
      if (s=="unsigned") uns=true; else if (s=="signed") sign=true;
      else if (s=="long") ++longs; else if (s=="short") sh=true;
      else if (s=="char") ischar=true; else if (s=="bool") isbool=true;
      else if (s=="char16_t" || s=="char32_t" || s=="wchar_t" || s=="nullptr_t") other=s;
      else if (s=="void") isvoid=true; else if (s=="float") isfloat=true;
      else if (s=="double") isdouble=true;
      else if (s=="int") {} else if (!s.empty()) other=s;
    }
    if (!other.empty()) {
      if (other=="char16_t" || other=="char32_t" || other=="wchar_t" || other=="nullptr_t") return builtin(other);
      return require_type(scope,other);
    }
    if (isvoid) return builtin("void");
    if (isbool) return builtin("bool");
    if (isfloat) return builtin("float");
    if (isdouble) return builtin(longs?"long double":"double");
    if (ischar) return builtin(uns?"unsigned char":sign?"signed char":"char");
    if (longs>=2) return builtin(uns?"unsigned long long int":"long long int");
    if (longs==1) return builtin(uns?"unsigned long int":"long int");
    if (sh) return builtin(uns?"unsigned short int":"short int");
    return builtin(uns?"unsigned int":"int");
  }
  int type_from_specifiers(const Node &spec,int scope,const std::string &anonymous_name="") {
    bool c=false,v=false,constexpr_flag=false;
    std::vector<std::string> fundamental;
    int result=-1;
    for (std::size_t i=0;i<spec.children.size();++i) {
      const Node &ch=spec.children[i];
      if (ch.text.compare(0,12,"cv-qualifier")==0) {
        if (ch.text.find("KW_CONST:const")!=std::string::npos) c=true;
        if (ch.text.find("KW_VOLATILE:volatile")!=std::string::npos) v=true;
      } else if (ch.text.compare(0,15,"decl-specifier ")==0) {
        std::string x=specifier_value(ch);
        if (x=="const") c=true;
        else if (x=="volatile") v=true;
        else if (x=="constexpr") constexpr_flag=true;
        else if (x=="typedef" || x=="extern" || x=="static" || x=="inline" ||
                x=="virtual" || x=="thread_local" || x=="friend" || x=="register" || x=="mutable") {}
        else if (x.compare(0,8,"decltype")==0) result=decltype_type(ch,scope);
        else if (x.find("::")!=std::string::npos ||
                 x=="auto" || x=="int" || x=="signed" || x=="unsigned" ||
                 x=="long" || x=="short" || x=="char" || x=="bool" ||
                 x=="char16_t" || x=="char32_t" || x=="wchar_t" ||
                 x=="nullptr_t" || x=="float" || x=="double" || x=="void") fundamental.push_back(x);
        else result=require_type(scope,x);
      } else if (ch.text.compare(0,15,"class-specifier")==0 ||
                 ch.text.compare(0,25,"class-forward-declaration")==0) {
        std::string n=ch.text.compare(0,16,"class-specifier ")==0?ch.text.substr(16):
                      ch.text.compare(0,26,"class-forward-declaration ")==0?ch.text.substr(26):anonymous_name;
        bool old_anon_union=allow_anon_union_;
        if(ch.text=="class-specifier" && class_key(ch)=="union" && anonymous_name.empty() &&
           scope>=0 && scopes_[scope].kind=="namespace" && has_spec(spec,"static")) allow_anon_union_=true;
        int owner=analyze_class(ch,scope,n);
        allow_anon_union_=old_anon_union;
        if (n.empty()) n=anonymous_name;
        if (n.empty()) n=name_text(scopes_[owner].name_id);
        result=class_type(owner,class_key(ch),n);
      } else if (ch.text.compare(0,14,"enum-specifier")==0) {
        std::string n=after_prefix(ch.text,"enum-specifier"); n=trim(n);
        if (n.empty()) n=anonymous_name;
        bool has_key=false, has_enumerator=false;
        for(std::size_t k=0;k<ch.children.size();++k) {
          if(ch.children[k].text.compare(0,9,"enum-key ")==0)has_key=true;
          if(ch.children[k].text.compare(0,11,"enumerator ")==0)has_enumerator=true;
        }
        std::pair<int,int> known=n.empty()?std::make_pair(-1,-1):resolve_name(scope,n,true,false);
        if(!has_key && !has_enumerator && known.first>=0) result=binding_type(known.first,known.second);
        else result=analyze_enum(ch,scope,n);
      } else if (ch.text.compare(0,9,"type-name")==0) {
        result=require_type(scope,trim(ch.text.substr(9)));
      }
    }
    if (result<0) result=fundamental_type(fundamental,scope);
    (void)constexpr_flag;
    return cv_type(result,c,v);
  }
  struct DeclaratorOp {
    int kind; // 0 pointer, 1 lref, 2 rref, 3 array, 4 function, 5 member pointer
    bool c,v,variadic;
    int owner_scope;
    unsigned long long bound;
    std::vector<int> params;
    DeclaratorOp() : kind(0),c(false),v(false),variadic(false),owner_scope(-1),bound(0) {}
  };
  void collect_ops(const Node &d,std::vector<DeclaratorOp> &ops,
                   std::string &name,int scope) {
    if (d.text!="declarator" && d.text!="abstract-declarator") return;
    std::vector<DeclaratorOp> prefix;
    const Node *nested=0;
    std::size_t i=0;
    while (i<d.children.size()) {
      const Node &ch=d.children[i];
      if (ch.text.compare(0,12,"ptr-operator")==0) {
        std::string op=ch.text.substr(13);
        DeclaratorOp x;
        if(op.size()>=3 && op.compare(op.size()-3,3,"::*")==0) {
          x.kind=5;
          std::string owner=op.substr(0,op.size()-3);
          int owner_type=require_type(scope,owner);
          while(owner_type>=0&&types_[owner_type].kind==TY_CV)owner_type=types_[owner_type].base;
          if(owner_type<0||types_[owner_type].kind!=TY_CLASS)throw std::runtime_error("member pointer owner is not a class");
          x.owner_scope=types_[owner_type].owner_scope;
        } else if (op.find("OP_LAND:&&")!=std::string::npos || op.find("OP_AMP:&&")!=std::string::npos || op=="&&") x.kind=2;
        else if (op.find("OP_AMP:&")!=std::string::npos || op=="&") x.kind=1;
        else x.kind=0;
        ++i;
        while (i<d.children.size() && d.children[i].text.compare(0,12,"cv-qualifier")==0) {
          std::string q=d.children[i].text;
          if (q.find("KW_CONST:const")!=std::string::npos || q=="cv-qualifier const") x.c=true;
          if (q.find("KW_VOLATILE:volatile")!=std::string::npos || q=="cv-qualifier volatile") x.v=true;
          ++i;
        }
        prefix.push_back(x); continue;
      }
      if (ch.text.compare(0,17,"nested-declarator")==0) {
        for (std::size_t k=0;k<ch.children.size();++k)
          if (ch.children[k].text=="declarator" || ch.children[k].text=="abstract-declarator") nested=&ch.children[k];
        ++i; continue;
      }
      if (ch.text.compare(0,11,"identifier ")==0) { name=ch.text.substr(11); ++i; continue; }
      ++i;
    }
    if (nested) collect_ops(*nested,ops,name,scope);
    // Suffixes compose outside a parenthesized declarator, but inside the
    // declarator's pointer prefix. This order distinguishes *p[3] from (*p)[3].
    for (std::size_t k=0;k<d.children.size();++k) {
      const Node &ch=d.children[k];
      if (ch.text=="array-suffix") {
        DeclaratorOp op; op.kind=3;
        if (!ch.children.empty()) {
          ConstValue n=eval(ch.children[0],scope);
          if (!n.valid || n.enum_id || n.value<=0) throw std::runtime_error("array bound must be a positive integral constant");
          op.bound=static_cast<unsigned long long>(n.value);
        }
        ops.push_back(op);
      } else if (ch.text=="parameter-clause") {
        DeclaratorOp op; op.kind=4;
        for (std::size_t q=0;q<ch.children.size();++q) {
          const Node &p=ch.children[q];
          if (p.text=="parameter-pack ...") { op.variadic=true; continue; }
          if (p.text!="parameter-declaration") continue;
          bool pack=false;
          for(std::size_t k=0;k<p.children.size();++k) {
            if(p.children[k].text=="parameter-pack ...")pack=true;
            if(p.children[k].text=="declarator" || p.children[k].text=="abstract-declarator")
              for(std::size_t z=0;z<p.children[k].children.size();++z)
                if(p.children[k].children[z].text=="parameter-pack ...")pack=true;
          }
          if(pack) op.variadic=true;
          int pt=parameter_type(p,scope);
          op.params.push_back(pt);
        }
        ops.push_back(op);
      } else if(ch.text.compare(0,std::string("cv-qualifier").size(),"cv-qualifier")==0 && !ops.empty() && ops.back().kind==4) {
        if(ch.text.find("KW_CONST:const")!=std::string::npos || ch.text.find(" const")!=std::string::npos)ops.back().c=true;
        if(ch.text.find("KW_VOLATILE:volatile")!=std::string::npos || ch.text.find(" volatile")!=std::string::npos)ops.back().v=true;
      }
    }
    for (std::size_t k=prefix.size();k>0;--k) ops.push_back(prefix[k-1]);
  }
  int derive_declarator(const Node &d,int base,int scope,std::string *out_name=0) {
    std::vector<DeclaratorOp> ops; std::string name;
    collect_ops(d,ops,name,scope);
    for (std::size_t i=ops.size();i>0;--i) {
      const DeclaratorOp &op=ops[i-1];
      if (op.kind==0) base=pointer_type(base,op.c,op.v);
      else if (op.kind==1) base=reference_type(base,false);
      else if (op.kind==2) base=reference_type(base,true);
      else if (op.kind==3) base=array_type(base,op.bound);
      else if (op.kind==4) base=function_type(base,op.params,op.variadic,op.c,op.v);
      else base=member_pointer_type(base,op.owner_scope);
    }
    if (out_name) *out_name=name;
    return base;
  }
  int parameter_type(const Node &parameter,int scope) {
    const Node *spec=0,*decl=0;
    for (std::size_t i=0;i<parameter.children.size();++i) {
      if (parameter.children[i].text=="decl-specifier-seq") spec=&parameter.children[i];
      if (parameter.children[i].text=="declarator" || parameter.children[i].text=="abstract-declarator") decl=&parameter.children[i];
    }
    if (!spec) throw std::runtime_error("parameter has no type");
    int base=type_from_specifiers(*spec,scope);
    if (decl) base=derive_declarator(*decl,base,scope);
    return base;
  }
  ConstValue eval_literal(const std::string &text) const {
    std::string x=text;
    if (x.compare(0,8,"literal ")==0) x=x.substr(8);
    if (x.compare(0,12,"TT_LITERAL:")==0) x=x.substr(12);
    if (x.empty()) return ConstValue();
    if (x[0]=='\'' || x.compare(0,2,"u'")==0 || x.compare(0,2,"U'")==0 || x.compare(0,2,"L'")==0) {
      std::size_t q=x.find('\''); if (q==std::string::npos || x.size()<q+2) return ConstValue();
      unsigned long long c=0;
      if (x[q+1]=='\\') {
        if (q+2>=x.size()) return ConstValue();
        char e=x[q+2];
        switch(e) {
        case 'n':c='\n';break; case 'r':c='\r';break; case 't':c='\t';break;
        case '0':c=0;break; case '\\':c='\\';break; case '\'':c='\'';break;
        case '"':c='"';break;
        case 'x': { char *end=0; errno=0; c=std::strtoull(x.c_str()+q+3,&end,16); if(errno||end==x.c_str()+q+3)return ConstValue(); break; }
        default: if(e>='0'&&e<='7') { c=e-'0'; for(std::size_t j=q+3;j<x.size()&&x[j]>='0'&&x[j]<='7';++j)c=c*8+(x[j]-'0'); } else c=static_cast<unsigned char>(e);
        }
      } else c=static_cast<unsigned char>(x[q+1]);
      return ConstValue(static_cast<long long>(c));
    }
    if (x=="true") return ConstValue(1);
    if (x=="false") return ConstValue(0);
    if (!(std::isdigit(static_cast<unsigned char>(x[0])))) return ConstValue();
    std::size_t end=x.size();
    while (end && (x[end-1]=='u'||x[end-1]=='U'||x[end-1]=='l'||x[end-1]=='L')) --end;
    std::string digits=x.substr(0,end); if(digits.empty())return ConstValue();
    char *p=0; errno=0; unsigned long long v=std::strtoull(digits.c_str(),&p,0);
    if(errno || p!=digits.c_str()+digits.size() || v>static_cast<unsigned long long>(LLONG_MAX)) return ConstValue();
    return ConstValue(static_cast<long long>(v));
  }
  std::string expression_operator(const Node &n,const std::string &prefix) const {
    std::string s=after_prefix(n.text,prefix);
    std::size_t p=s.find(':'); return p==std::string::npos?s:s.substr(p+1);
  }
  ConstValue lookup_constant(int scope,const std::string &name,
                             int enum_initializer_context=0) const {
    std::pair<int,int> found=resolve_name(scope,name,false,false);
    if (found.first<0) throw std::runtime_error("unknown value in constant expression: "+name);
    const Binding &b=binding(found.first,found.second);
    if (!b.has_value) throw std::runtime_error("not an integral constant expression");
    // Within an enumerator initializer, earlier enumerators of this same
    // enumeration are integral constant values for the initializer rules.
    const int enum_id=b.enum_id==enum_initializer_context?0:b.enum_id;
    return ConstValue(b.value,enum_id);
  }
  static bool add_overflow(long long a,long long b,long long &r) {
    if ((b>0 && a>LLONG_MAX-b)||(b<0 && a<LLONG_MIN-b)) return true;
    r=a+b; return false;
  }
  static bool sub_overflow(long long a,long long b,long long &r) {
    if ((b<0 && a>LLONG_MAX+b)||(b>0 && a<LLONG_MIN+b)) return true;
    r=a-b; return false;
  }
  static bool mul_overflow(long long a,long long b,long long &r) {
    if (a==0||b==0) {r=0;return false;}
    if ((a==LLONG_MIN&&b==-1)||(b==LLONG_MIN&&a==-1))return true;
    if (a>0 ? (b>0 ? a>LLONG_MAX/b : b<LLONG_MIN/a)
             : (b>0 ? a<LLONG_MIN/b : a!=0&&b<LLONG_MAX/a)) return true;
    r=a*b;return false;
  }
  bool const_integral_value_type(int type) const {
    while(type>=0 && (types_[type].kind==TY_CV ||
                      types_[type].kind==TY_LREF || types_[type].kind==TY_RREF))
      type=types_[type].base;
    if(type<0)return false;
    if(types_[type].kind==TY_ENUM)return true;
    if(types_[type].kind!=TY_BUILTIN)return false;
    const std::string &name=name_text(types_[type].name_id);
    return name=="bool" || name=="char" || name=="signed char" ||
           name=="unsigned char" || name=="wchar_t" || name=="char16_t" ||
           name=="char32_t" || name=="short int" || name=="unsigned short int" ||
           name=="int" || name=="unsigned int" || name=="long int" ||
           name=="unsigned long int" || name=="long long int" ||
           name=="unsigned long long int";
  }
  bool const_qualified_value_type(int type) const {
    if(type>=0 && (types_[type].kind==TY_LREF || types_[type].kind==TY_RREF))
      type=types_[type].base;
    return type>=0 && types_[type].kind==TY_CV && types_[type].is_const;
  }
  int scoped_enum_id_of_type(int type) const {
    while(type>=0 && types_[type].kind==TY_CV)type=types_[type].base;
    return type>=0 && types_[type].kind==TY_ENUM && types_[type].scoped?
           types_[type].owner_scope+1:0;
  }
  int expression_scoped_enum_id(const Node &n,int scope,
                                int enum_initializer_context=0) {
    const std::string &kind=n.text;
    if(kind.compare(0,8,"literal ")==0 || kind.compare(0,12,"TT_LITERAL:")==0)
      return 0;
    if(kind.compare(0,14,"id-expression ")==0) {
      std::pair<int,int> f=resolve_name(scope,kind.substr(14),false,false);
      if(f.first<0)throw std::runtime_error("unknown value in constant expression: "+kind.substr(14));
      const Binding &b=binding(f.first,f.second);
      if(enum_initializer_context && b.enum_id==enum_initializer_context)return 0;
      return b.enum_id?b.enum_id:scoped_enum_id_of_type(b.type);
    }
    if(kind=="parenthesized-expression" && !n.children.empty())
      return expression_scoped_enum_id(n.children[0],scope,enum_initializer_context);
    if(kind.compare(0,22,"sizeof-expression")==0 ||
       kind.compare(0,22,"type-trait-expression ")==0) {
      if(!n.children.empty()) {
        const int type=type_id_type(n.children[0],scope);
        if(kind.compare(0,22,"sizeof-expression")==0)type_size(type,scope);
        else type_alignment(type,scope);
      }
      return 0;
    }
    if(kind.compare(0,15,"cast-expression")==0 && n.children.size()>=2)
      return scoped_enum_id_of_type(type_id_type(n.children[0],scope));
    if(kind.compare(0,17,"unary-expression ")==0 && !n.children.empty()) {
      const int operand=expression_scoped_enum_id(n.children[0],scope,enum_initializer_context);
      if(operand)throw std::runtime_error("scoped enum requires explicit conversion");
      return 0;
    }
    if(kind.compare(0,18,"binary-expression ")==0 && n.children.size()>=2) {
      const int lhs=expression_scoped_enum_id(n.children[0],scope,enum_initializer_context);
      const int rhs=expression_scoped_enum_id(n.children[1],scope,enum_initializer_context);
      const std::string op=expression_operator(n,"binary-expression ");
      if(op=="&&" || op=="||") {
        if(lhs || rhs)throw std::runtime_error("scoped enum is not contextually convertible to bool");
      } else if(op=="==" || op=="!=") {
        if((lhs && !rhs)||(!lhs && rhs)||(lhs && rhs && lhs!=rhs))
          throw std::runtime_error("invalid scoped-enum comparison");
      } else if(op=="<" || op==">" || op=="<=" || op==">=") {
        if(lhs || rhs)throw std::runtime_error("invalid scoped-enum comparison");
      } else if(lhs || rhs) {
        throw std::runtime_error("scoped enum requires explicit conversion");
      }
      return 0;
    }
    if(kind.compare(0,22,"conditional-expression")==0 && n.children.size()>=3) {
      const int condition=expression_scoped_enum_id(n.children[0],scope,enum_initializer_context);
      if(condition)throw std::runtime_error("scoped enum is not contextually convertible to bool");
      const int yes=expression_scoped_enum_id(n.children[1],scope,enum_initializer_context);
      const int no=expression_scoped_enum_id(n.children[2],scope,enum_initializer_context);
      if(yes && no && yes!=no)throw std::runtime_error("incompatible scoped enum operands");
      if((yes && !no)||(!yes && no))throw std::runtime_error("scoped enum requires explicit conversion");
      return yes?yes:no;
    }
    return 0;
  }
  ConstValue eval(const Node &n,int scope,int enum_initializer_context=0) {
    std::string kind=n.text;
    if (kind.compare(0,8,"literal ")==0 || kind.compare(0,12,"TT_LITERAL:")==0)
      return eval_literal(kind);
    if (kind.compare(0,24,"parenthesized-expression")==0) {
      if (n.children.empty()) return ConstValue();
      return eval(n.children[0],scope,enum_initializer_context);
    }
    if (kind.compare(0,14,"id-expression ")==0)
      return lookup_constant(scope,kind.substr(14),enum_initializer_context);
    if (kind.compare(0,18,"binary-expression ")==0 && n.children.size()>=2) {
      std::string op=expression_operator(n,"binary-expression ");
      ConstValue a=eval(n.children[0],scope,enum_initializer_context);
      if((op=="&&" || op=="||") && a.enum_id)
        throw std::runtime_error("scoped enum is not contextually convertible to bool");
      if (op=="&&" && !a.value) {
        if(expression_scoped_enum_id(n.children[1],scope,enum_initializer_context))
          throw std::runtime_error("scoped enum is not contextually convertible to bool");
        return ConstValue(0);
      }
      if (op=="||" && a.value) {
        if(expression_scoped_enum_id(n.children[1],scope,enum_initializer_context))
          throw std::runtime_error("scoped enum is not contextually convertible to bool");
        return ConstValue(1);
      }
      ConstValue b=eval(n.children[1],scope,enum_initializer_context);
      if ((op=="==" || op=="!=" || op=="<" || op==">" || op=="<=" || op==">=") &&
          ((a.enum_id && !b.enum_id)||(b.enum_id && !a.enum_id)||(a.enum_id && b.enum_id && a.enum_id!=b.enum_id)))
        throw std::runtime_error("invalid scoped-enum comparison");
      const bool equality=(op=="==" || op=="!=");
      if((a.enum_id || b.enum_id) &&
         !(equality && a.enum_id && a.enum_id==b.enum_id))
        throw std::runtime_error("scoped enum requires explicit conversion");
      long long r=0;
      if (op=="+") {if(add_overflow(a.value,b.value,r))throw std::runtime_error("constant overflow");return ConstValue(r);}
      if (op=="-") {if(sub_overflow(a.value,b.value,r))throw std::runtime_error("constant overflow");return ConstValue(r);}
      if (op=="*") {if(mul_overflow(a.value,b.value,r))throw std::runtime_error("constant overflow");return ConstValue(r);}
      if (op=="/") {if(!b.value|| (a.value==LLONG_MIN&&b.value==-1))throw std::runtime_error("invalid constant division");return ConstValue(a.value/b.value);}
      if (op=="%") {if(!b.value || (a.value==LLONG_MIN&&b.value==-1))throw std::runtime_error("invalid constant remainder");return ConstValue(a.value%b.value);}
      if (op=="&&") return ConstValue(a.value&&b.value);
      if (op=="||") return ConstValue(a.value||b.value);
      if (op=="==") return ConstValue(a.value==b.value);
      if (op=="!=") return ConstValue(a.value!=b.value);
      if (op=="<") return ConstValue(a.value<b.value);
      if (op==">") return ConstValue(a.value>b.value);
      if (op=="<=") return ConstValue(a.value<=b.value);
      if (op==">=") return ConstValue(a.value>=b.value);
      if (op=="&") return ConstValue(a.value&b.value);
      if (op=="|") return ConstValue(a.value|b.value);
      if (op=="^") return ConstValue(a.value^b.value);
      if (op=="<<") {if(b.value<0||b.value>=63||a.value<0||a.value>(LLONG_MAX>>b.value))throw std::runtime_error("constant shift overflow");return ConstValue(a.value<<b.value);}
      if (op==">>") {if(b.value<0||b.value>=63)throw std::runtime_error("invalid constant shift");return ConstValue(a.value>>b.value);}
      throw std::runtime_error("unsupported constant operator");
    }
    if (kind.compare(0,17,"unary-expression ")==0 && !n.children.empty()) {
      std::string op=expression_operator(n,"unary-expression ");
      ConstValue a=eval(n.children[0],scope,enum_initializer_context);
      if(a.enum_id)throw std::runtime_error("scoped enum requires explicit conversion");
      if(op=="+")return a;
      if(op=="-") { if(a.value==LLONG_MIN)throw std::runtime_error("constant overflow"); return ConstValue(-a.value); }
      if(op=="!")return ConstValue(!a.value);
      if(op=="~")return ConstValue(~a.value);
    }
    if (kind.compare(0,22,"conditional-expression")==0 && n.children.size()>=3) {
      ConstValue c=eval(n.children[0],scope,enum_initializer_context);
      if(c.enum_id)throw std::runtime_error("scoped enum is not contextually convertible to bool");
      return eval(n.children[c.value?1:2],scope,enum_initializer_context);
    }
    if (kind.compare(0,17,"sizeof-expression")==0 || kind.compare(0,22,"type-trait-expression ")==0) {
      if(n.children.empty()) return ConstValue();
      int t=type_id_type(n.children[0],scope);
      const bool alignment=kind.find("ALIGNOF")!=std::string::npos || kind.find("alignof")!=std::string::npos;
      return ConstValue(alignment?type_alignment(t,scope):type_size(t,scope));
    }
    if (kind.compare(0,15,"cast-expression")==0 && n.children.size()>=2) {
      int t=type_id_type(n.children[0],scope);
      ConstValue v=eval(n.children.back(),scope,enum_initializer_context);
      if(types_[t].kind==TY_ENUM && types_[t].scoped) return ConstValue(v.value,types_[t].owner_scope+1);
      return ConstValue(v.value);
    }
    throw std::runtime_error("not an integral constant expression: "+kind);
  }
  static long long round_up(long long value,long long alignment) {
    if(alignment<=0)throw std::runtime_error("invalid object alignment");
    const long long remainder=value%alignment;
    if(!remainder)return value;
    if(value>LLONG_MAX-(alignment-remainder))throw std::runtime_error("object layout overflow");
    return value+(alignment-remainder);
  }
  void complete_class_layout(int scope) {
    Scope &s=scopes_[scope];
    if(s.layout_computed)return;
    if(!s.visible || !s.layout_complete || s.layout_in_progress || s.layout_unsupported)
      throw std::runtime_error("class layout is incomplete or unsupported");
    s.layout_in_progress=true;
    long long offset=0, max_align=1, max_size=0;
    bool inherited_vptr=false, empty_bases=true, has_data=false;
    for(std::size_t i=0;i<s.base_types.size();++i) {
      int base=s.base_types[i];
      while(types_[base].kind==TY_CV)base=types_[base].base;
      if(types_[base].kind!=TY_CLASS)throw std::runtime_error("invalid base class type");
      const int base_scope=types_[base].owner_scope;
      complete_class_layout(base_scope);
      const Scope &bs=scopes_[base_scope];
      empty_bases=empty_bases && bs.layout_empty;
      inherited_vptr=inherited_vptr || bs.layout_has_vptr;
      max_align=std::max(max_align,bs.object_alignment);
      // Empty base optimization and reuse of base tail padding follow the
      // target C++ ABI; nonempty bases retain their source declaration order.
      if(!bs.layout_empty) {
        offset=round_up(offset,bs.object_alignment);
        if(bs.object_data_size>LLONG_MAX-offset)throw std::runtime_error("object layout overflow");
        offset+=bs.object_data_size;
      }
    }
    if(s.has_virtual && !inherited_vptr) {
      // A class with virtual functions but no primary virtual base owns its
      // vptr at offset zero on the target x86-64 C++ ABI.
      if(offset==0)offset=8;
      else offset=round_up(offset,8)+8;
      max_align=std::max<long long>(max_align,8);
    }
    FlatMap<int,bool> laid_out_entities;
    for(std::size_t i=0;i<s.bindings.size();++i) {
      const Binding &b=s.bindings[i];
      if(b.kind!="variable")continue;
      if(b.entity_id>=0) {
        if(laid_out_entities.find(b.entity_id))continue;
        laid_out_entities[b.entity_id]=true;
      }
      if(b.is_static)continue;
      has_data=true;
      // On the target x86-64 ABI a reference data member occupies a pointer
      // slot, even though sizeof(reference-type) reports the referent size.
      const bool reference_member=b.type>=0 &&
          (types_[b.type].kind==TY_LREF || types_[b.type].kind==TY_RREF);
      const long long size=reference_member?8:type_size(b.type,scope);
      const long long align=reference_member?8:type_alignment(b.type,scope);
      if(s.union_class) max_size=std::max(max_size,size);
      else {
        offset=round_up(offset,align);
        if(size>LLONG_MAX-offset)throw std::runtime_error("object layout overflow");
        offset+=size;
      }
      max_align=std::max(max_align,align);
    }
    if(s.union_class)offset=max_size;
    s.layout_has_vptr=s.has_virtual || inherited_vptr;
    s.layout_empty=!s.union_class && !has_data && !s.layout_has_vptr && empty_bases;
    if(offset==0)offset=1;
    s.object_alignment=max_align;
    s.object_data_size=offset;
    s.object_size=round_up(offset,max_align);
    s.layout_in_progress=false;
    s.layout_computed=true;
  }
  long long type_size(int type,int scope) {
    if(type<0)throw std::runtime_error("invalid type size");
    const Type &t=types_[type];
    if(t.kind==TY_CV)return type_size(t.base,scope);
    if(t.kind==TY_POINTER)return 8;
    if(t.kind==TY_LREF || t.kind==TY_RREF)return type_size(t.base,scope);
    if(t.kind==TY_ARRAY) {
      if(!t.bound)throw std::runtime_error("sizeof incomplete array");
      const long long el=type_size(t.base,scope);
      if(t.bound>static_cast<unsigned long long>(LLONG_MAX/el))throw std::runtime_error("size overflow");
      return static_cast<long long>(t.bound)*el;
    }
    if(t.kind==TY_ENUM) {
      const int *underlying=enum_underlying_.find(t.owner_scope);
      return underlying?type_size(*underlying,scope):4;
    }
    if(t.kind==TY_CLASS) {
      complete_class_layout(t.owner_scope);
      return scopes_[t.owner_scope].object_size;
    }
    if(t.kind==TY_FUNCTION)throw std::runtime_error("sizeof function");
    const std::string &n=name_text(t.name_id);
    if(n=="char"||n=="signed char"||n=="unsigned char"||n=="bool"||n=="char8_t")return 1;
    if(n=="char16_t"||n=="short int"||n=="unsigned short int")return 2;
    if(n=="char32_t"||n=="wchar_t"||n=="int"||n=="unsigned int"||n=="float")return 4;
    if(n=="long int"||n=="unsigned long int"||n=="long long int"||n=="unsigned long long int"||n=="double")return 8;
    if(n=="long double")return 16;
    if(n=="nullptr_t")return 8;
    throw std::runtime_error("unknown type size");
  }
  long long type_alignment(int type,int scope) {
    if(type<0)throw std::runtime_error("invalid type alignment");
    const Type &t=types_[type];
    if(t.kind==TY_CV)return type_alignment(t.base,scope);
    if(t.kind==TY_POINTER)return 8;
    if(t.kind==TY_LREF || t.kind==TY_RREF)return type_size(t.base,scope);
    if(t.kind==TY_ARRAY)return type_alignment(t.base,scope);
    if(t.kind==TY_ENUM) {
      const int *underlying=enum_underlying_.find(t.owner_scope);
      return underlying?type_alignment(*underlying,scope):4;
    }
    if(t.kind==TY_CLASS) {
      complete_class_layout(t.owner_scope);
      return scopes_[t.owner_scope].object_alignment;
    }
    if(t.kind==TY_FUNCTION)throw std::runtime_error("alignof function");
    const std::string &n=name_text(t.name_id);
    if(n=="char"||n=="signed char"||n=="unsigned char"||n=="bool"||n=="char8_t")return 1;
    if(n=="char16_t"||n=="short int"||n=="unsigned short int")return 2;
    if(n=="char32_t"||n=="wchar_t"||n=="int"||n=="unsigned int"||n=="float")return 4;
    if(n=="long int"||n=="unsigned long int"||n=="long long int"||n=="unsigned long long int"||n=="double")return 8;
    if(n=="long double")return 16;
    if(n=="nullptr_t")return 8;
    throw std::runtime_error("unknown type alignment");
  }
  int type_id_type(const Node &node,int scope) {
    const Node *seq=0;
    if(node.text=="type-id") {
      for(std::size_t i=0;i<node.children.size();++i)
        if(node.children[i].text=="type-specifier-seq" || node.children[i].text=="decl-specifier-seq") seq=&node.children[i];
      if(!seq) throw std::runtime_error("invalid type-id");
      Node fake("decl-specifier-seq");
      for(std::size_t i=0;i<seq->children.size();++i) {
        std::string text=seq->children[i].text;
        if(text.compare(0,14,"type-specifier")==0) text="decl-specifier"+text.substr(14);
        else if(text.compare(0,18,"decltype-specifier")==0)
          text="decl-specifier"+text.substr(18);
        Node item(text); for(std::size_t j=0;j<seq->children[i].children.size();++j)item.children.push_back(seq->children[i].children[j]);
        fake.children.push_back(item);
      }
      int base=type_from_specifiers(fake,scope);
      for(std::size_t i=0;i<node.children.size();++i)
        if(node.children[i].text=="abstract-declarator") base=derive_declarator(node.children[i],base,scope);
      return base;
    }
    if(node.text=="type-specifier-seq" || node.text=="decl-specifier-seq") {
      Node fake("decl-specifier-seq");
      for(std::size_t i=0;i<node.children.size();++i) {
        std::string text=node.children[i].text;
        if(text.compare(0,14,"type-specifier")==0) text="decl-specifier"+text.substr(14);
        else if(text.compare(0,18,"decltype-specifier")==0)
          text="decl-specifier"+text.substr(18);
        Node item(text); for(std::size_t j=0;j<node.children[i].children.size();++j)item.children.push_back(node.children[i].children[j]);
        fake.children.push_back(item);
      }
      return type_from_specifiers(fake,scope);
    }
    return type_from_specifiers(node,scope);
  }
  int decltype_type(const Node &spec,int scope) {
    if(spec.children.empty()) return builtin("int");
    const Node &expr=spec.children[0];
    if(expr.text=="keyword-literal KW_NULLPTR:nullptr")return builtin("nullptr_t");
    if(expr.text=="sizeof-expression")return builtin("unsigned long int");
    if(expr.text=="cast-expression" && !expr.children.empty() && expr.children[0].text=="type-id" &&
       !expr.children[0].children.empty() && !expr.children[0].children[0].children.empty() &&
       expr.children[0].children[0].children[0].text.compare(0,10,"type-name ")==0) {
      std::string name=expr.children[0].children[0].children[0].text.substr(10);
      std::pair<int,int> f=resolve_name(scope,name,false,false);
      if(f.first>=0) {
        int type=binding_type(f.first,f.second);while(type>=0&&types_[type].kind==TY_CV)type=types_[type].base;
        if(type>=0&&types_[type].kind==TY_FUNCTION)return types_[type].base;
      }
    }
    if(expr.text=="call-expression" && !expr.children.empty() &&
       expr.children[0].text.compare(0,14,"id-expression ")==0) {
      std::string name=expr.children[0].text.substr(14);
      std::pair<int,int> f=resolve_name(scope,name,false,false);
      if(f.first>=0) {
        const Binding &b=binding(f.first,f.second);
        int type=b.type;while(type>=0&&types_[type].kind==TY_CV)type=types_[type].base;
        if(type>=0&&types_[type].kind==TY_FUNCTION)return types_[type].base;
      }
    }
    if(expr.text.compare(0,8,"literal ")==0)return builtin("int");
    if(expr.text.compare(0,15,"binary-expression")==0) {
      if(expr.text.find("OP_EQ")!=std::string::npos || expr.text.find("OP_NE")!=std::string::npos ||
         expr.text.find("OP_LT")!=std::string::npos || expr.text.find("OP_GT")!=std::string::npos ||
         expr.text.find("OP_LE")!=std::string::npos || expr.text.find("OP_GE")!=std::string::npos ||
         expr.text.find("OP_LAND")!=std::string::npos || expr.text.find("OP_LOR")!=std::string::npos)return builtin("bool");
      return builtin("int");
    }
    if(expr.text.compare(0,14,"id-expression ")==0) {
      std::string name=expr.text.substr(14);
      std::pair<int,int> f=resolve_name(scope,name,false,false);
      if(f.first<0) throw std::runtime_error("unknown decltype name");
      const Binding &b=binding(f.first,f.second);
      return b.type;
    }
    if(expr.text=="parenthesized-expression" && !expr.children.empty()) {
      const Node &inner=expr.children[0];
      if(inner.text.compare(0,14,"id-expression ")==0) {
        std::pair<int,int> f=resolve_name(scope,inner.text.substr(14),false,false);
        if(f.first<0) throw std::runtime_error("unknown decltype name");
        const Binding &b=binding(f.first,f.second);
        if(b.kind=="variable" || b.kind=="parameter") return reference_type(b.type,false);
        return b.type;
      }
    }
    throw std::runtime_error("unsupported decltype form");
  }
  FlatMap<int,int> enum_underlying_;
  FlatMap<int,int> enum_definition_lookup_;
  int scope_for_qualified_name(int scope,const std::string &qualified,
                               std::string *member=0) {
    std::vector<std::string> parts=split_qualified(qualified);
    if(parts.empty()) return scope;
    std::string last=parts.back(); parts.pop_back();
    int current=scope;
    if(qualified.compare(0,2,"::")==0) current=root_scope(scope);
    for(std::size_t i=0;i<parts.size();++i) {
      std::pair<int,int> f=qualified_scope_component(current,parts[i]);
      if(f.first<0) throw std::runtime_error("unknown qualified scope: "+parts[i]);
      int target=namespace_target(f.first,f.second);
      if(target<0) {
        int ty=binding_type(f.first,f.second);
        if(ty<0)throw std::runtime_error("invalid qualified scope");
        while(types_[ty].kind==TY_CV)ty=types_[ty].base;
        if(types_[ty].kind!=TY_CLASS && types_[ty].kind!=TY_ENUM)throw std::runtime_error("invalid qualifier");
        target=types_[ty].owner_scope;
      }
      if(target<0)throw std::runtime_error("invalid scope target");
      current=target;
    }
    if(member)*member=last;
    return current;
  }
  bool is_ancestor_scope(int ancestor,int child) const {
    for(int p=child;p>=0;p=scopes_[p].parent) if(p==ancestor)return true;
    return false;
  }
  int ensure_named_type_scope(int parent,const std::string &kind,
                              const std::string &name,bool visible,
                              const SyntaxLocation &location=SyntaxLocation()) {
    int found=find_child_scope(parent,kind,name);
    if(found>=0) { if(visible) scopes_[found].visible=true; return found; }
    const int id=add_scope(kind,name,parent,visible,location);
    scopes_[id].entity_id=add_entity(parent,intern_name(name),kind,-1,-1,location);
    return id;
  }
  std::string source_point_name(const Node &n,const std::string &prefix) const {
    std::size_t line=1,column=1;
    const std::uint32_t file=n.location.valid()?n.location.file_id():0;
    if(n.location.valid() && file<tree_.source_files.size()) {
      const std::shared_ptr<const std::string> source=tree_.source_files[file].contents;
      const std::size_t offset=n.location.source_offset();
      if(source) {
        std::vector<std::size_t> &starts=source_line_starts_[file];
        if(starts.empty()) {
          starts.push_back(0);
          for(std::size_t i=0;i<source->size();++i)
            if((*source)[i]=='\n')starts.push_back(i+1);
        }
        std::vector<std::size_t>::const_iterator after=
            std::upper_bound(starts.begin(),starts.end(),offset);
        const std::size_t index=after==starts.begin()?0:
            static_cast<std::size_t>((after-starts.begin())-1);
        line=index+1;
        column=offset-starts[index]+1;
      }
    }
    std::ostringstream out;
    out<<prefix;
    // Preserve legacy names for the main source while disambiguating anonymous
    // types whose identical line/column occurs in a separately included file.
    if(file)out<<"f"<<(file+1)<<"_";
    out<<line<<"_"<<column;
    return out.str();
  }
  int analyze_class(const Node &node,int scope,const std::string &forced_name="") {
    std::string full_name;
    if(node.text.compare(0,16,"class-specifier ")==0) full_name=node.text.substr(16);
    else if(node.text.compare(0,26,"class-forward-declaration ")==0) full_name=node.text.substr(26);
    if(full_name.empty()) full_name=forced_name;
    const bool anonymous=!full_name.empty()?false:(forced_name.empty());
    const std::string key=class_key(node);
    if(full_name.empty()) {
      if(key=="union") {
        std::ostringstream synthetic;
        const unsigned file=node.location.valid()?node.location.file_id()+1:1;
        synthetic<<"__anonymous_union_type__"<<file<<"_"<<(10+anonymous_union_serial_++);
        full_name=synthetic.str();
      } else full_name=source_point_name(node,"__anonymous_class__");
    }
    std::string name=tail_name(full_name);
    int owner=scope;
    const bool is_forward=node.text.compare(0,25,"class-forward-declaration")==0;
    if(full_name.find("::")!=std::string::npos)
      owner=scope_for_qualified_name(scope,full_name);
    else if(is_forward) {
      // Elaborated class lookup can find a class-name hidden by an ordinary
      // declaration; search lexical scopes for the class entity, not the
      // latest value binding with the same spelling.
      const int tag_name_id=find_name_id(name);
      bool found_tag=false;
      for(int here=scope;here>=0 && !found_tag;here=scopes_[here].parent) {
        if(tag_name_id<0)break;
        const BindingIndices *candidates=scopes_[here].by_name.find(
            static_cast<std::uint32_t>(tag_name_id));
        if(!candidates)continue;
        for(std::size_t i=candidates->size();i>0;--i) {
          const Binding &candidate=scopes_[here].bindings[(*candidates)[i-1]];
          if(candidate.kind!="type" || candidate.type<0)continue;
          int candidate_type=candidate.type;
          while(types_[candidate_type].kind==TY_CV)candidate_type=types_[candidate_type].base;
          if(types_[candidate_type].kind==TY_CLASS) {
            const int class_scope=types_[candidate_type].owner_scope;
            if(class_scope>=0)owner=scopes_[class_scope].parent;
            found_tag=true;
            break;
          }
        }
      }
    }
    bool definition=node.text.compare(0,15,"class-specifier")==0 && !is_forward;
    if(!definition && !is_forward)
      throw std::runtime_error("invalid class declaration");
    if(anonymous && key=="union" && scopes_[scope].kind=="namespace" && !allow_anon_union_)
      throw std::runtime_error("namespace-scope anonymous union requires static");
    int cls=ensure_named_type_scope(owner,"class",name,definition,node.location);
    scopes_[cls].union_class=(key=="union");
    for(std::size_t i=0;i<node.children.size();++i) {
      if(node.children[i].text=="bit-field-declaration")
        scopes_[cls].layout_unsupported=true;
      if(node.children[i].text=="base-clause") {
        for(std::size_t j=0;j<node.children[i].children.size();++j) {
          const Node &base=node.children[i].children[j];
          if(base.text!="base-specifier")continue;
          std::string base_name;
          bool virtual_base=false;
          for(std::size_t k=0;k<base.children.size();++k) {
            if(base.children[k].text.compare(0,10,"base-name ")==0)
              base_name=trim(base.children[k].text.substr(10));
            if(base.children[k].text.find("virtual")!=std::string::npos)
              virtual_base=true;
          }
          if(virtual_base)scopes_[cls].layout_unsupported=true;
          if(!base_name.empty()) {
            int base_type=require_type(owner,base_name);
            while(types_[base_type].kind==TY_CV)base_type=types_[base_type].base;
            if(types_[base_type].kind!=TY_CLASS)
              throw std::runtime_error("base specifier does not name a class");
            scopes_[cls].base_types.push_back(base_type);
          }
        }
      }
      const Node &declaration=node.children[i];
      for(std::size_t j=0;j<declaration.children.size();++j) {
        const Node &child=declaration.children[j];
        if(child.text!="decl-specifier-seq")continue;
        for(std::size_t k=0;k<child.children.size();++k)
          if(specifier_value(child.children[k])=="virtual")scopes_[cls].has_virtual=true;
      }
    }
    int ty=class_type(cls,key,name);
    bool has_type=false;
    int name_id=find_name_id(name);
    const BindingIndices *existing=name_id<0?0:scopes_[owner].by_name.find(static_cast<std::uint32_t>(name_id));
    if(existing) for(std::size_t i=0;i<existing->size();++i) {
      const Binding &old=scopes_[owner].bindings[(*existing)[i]];
      if(old.kind=="type")has_type=true;
    }
    bool emit_type=!anonymous && (definition || !has_type);
    if(emit_type) {
      int type_binding=add_binding(owner,name,"type",ty,-1,scopes_[cls].entity_id,node.location);
      Binding &display=scopes_[owner].bindings[type_binding];
      display.display_name_id=display.name_id;
      display.display_kind=key=="class"?DISPLAY_CLASS:key=="struct"?DISPLAY_STRUCT:DISPLAY_UNION;
    }
    if(!definition) return cls;

    std::vector<std::pair<const Node *,int> > deferred;
    for(std::size_t i=0;i<node.children.size();++i) {
      const Node &child=node.children[i];
      if(child.text=="class-key" || child.text=="base-clause" || child.text=="access-specifier" || child.text=="empty-declaration")continue;
      if(child.text=="function-definition") {
        int fn_scope=process_function(child,cls,true);
        if(fn_scope>=0) deferred.push_back(std::make_pair(&child,fn_scope));
      } else process_declaration(child,cls);
    }
    // Complete-class lookup sees later member types. Function scopes are made
    // in source order above, but bodies are semantically walked after members.
    scopes_[cls].layout_complete=true;
    for(std::size_t i=0;i<deferred.size();++i)
      process_function_body(*deferred[i].first,deferred[i].second,cls);
    if(anonymous && key=="union" && scopes_[scope].kind=="namespace") {
      for(std::size_t i=0;i<scopes_[cls].bindings.size();++i) {
        const Binding &member=scopes_[cls].bindings[i];
        if(member.kind=="variable" || member.kind=="function" || member.kind=="enumerator") {
          int injected=add_binding(scope,name_text(member.name_id),member.kind,
                                   member.type,-1,member.entity_id,member.location);
          scopes_[scope].bindings[injected].value=member.value;
          scopes_[scope].bindings[injected].has_value=member.has_value;
          scopes_[scope].bindings[injected].enum_id=member.enum_id;
        }
      }
    }
    return cls;
  }
  int analyze_enum(const Node &node,int scope,const std::string &given_name="") {
    std::string full_name=after_prefix(node.text,"enum-specifier");
    full_name=trim(full_name); if(full_name.empty())full_name=given_name;
    std::string name=tail_name(full_name);
    bool scoped=false;
    for(std::size_t i=0;i<node.children.size();++i)
      if(node.children[i].text.compare(0,9,"enum-key ")==0 &&
         (node.children[i].text.find("class")!=std::string::npos || node.children[i].text.find("struct")!=std::string::npos)) scoped=true;
    int owner=scope;
    bool qualified=full_name.find("::")!=std::string::npos;
    if(qualified) owner=scope_for_qualified_name(scope,full_name);
    bool body=false;
    for(std::size_t i=0;i<node.children.size();++i)
      if(node.children[i].text.compare(0,11,"enumerator ")==0)body=true;
    bool opaque=!body;
    if(name.empty() && !given_name.empty())name=given_name;
    if(name.empty())name=source_point_name(node,"__anonymous_enum__");
    int enum_scope=-1, ty=-1;
    int prior=latest_binding(owner,name);
    if(prior>=0 && (binding(owner,prior).kind=="type" || binding(owner,prior).kind=="type-alias")) {
      int existing=binding_type(owner,prior);
      while(existing>=0&&types_[existing].kind==TY_CV)existing=types_[existing].base;
      if(existing>=0 && types_[existing].kind==TY_ENUM) {
        ty=existing; enum_scope=types_[existing].owner_scope;
        if(types_[existing].scoped!=scoped && !qualified)throw std::runtime_error("incompatible enum redeclaration");
      }
    }
    if(enum_scope<0) {
      std::string scope_name=qualified?full_name:name;
      int output_parent=qualified?root_scope(scope):owner;
      enum_scope=ensure_named_type_scope(output_parent,"enum",scope_name,scoped,node.location);
      ty=enum_type(enum_scope,scope_name,scoped);
    } else if(scoped) scopes_[enum_scope].visible=true;
    if(opaque && !scoped) throw std::runtime_error("opaque unscoped enum not supported");
    if(!qualified) {
      add_binding(owner,name,"type",ty,-1,scopes_[enum_scope].entity_id,node.location);
    }
    int enumerator_scope=enum_scope;
    if(qualified && body) {
      enumerator_scope=ensure_named_type_scope(root_scope(scope),"enum",full_name,true,node.location);
      enum_definition_lookup_[ty]=enumerator_scope;
      int root_type=add_binding(root_scope(scope),full_name,"type",ty,-1,
                                scopes_[enum_scope].entity_id,node.location);
      Binding &display=scopes_[root_scope(scope)].bindings[root_type];
      display.display_name_id=intern_name(full_name);
      display.display_kind=scoped?DISPLAY_ENUM_CLASS:DISPLAY_ENUM;
    }
    int underlying=-1;
    for(std::size_t i=0;i<node.children.size();++i) {
      if(node.children[i].text=="type-specifier-seq" || node.children[i].text=="type-id")
        underlying=type_id_type(node.children[i],scope);
    }
    if(underlying>=0) {
      int *found=enum_underlying_.find(enum_scope);
      if(found && *found!=underlying)
        throw std::runtime_error("incompatible enum underlying type");
      enum_underlying_[enum_scope]=underlying;
    }
    long long next=0;
    for(std::size_t i=0;i<node.children.size();++i) {
      const Node &e=node.children[i];
      if(e.text.compare(0,11,"enumerator ")!=0)continue;
      std::string en=e.text.substr(11);
      long long value=next;
      if(!e.children.empty()) {
        ConstValue v=eval(e.children[0],scoped?enumerator_scope:owner,
                          scoped?enum_scope+1:0);
        if(!v.valid)throw std::runtime_error("invalid enumerator initializer");
        value=v.value;
      }
      int target=scoped?enumerator_scope:owner;
      int bid=add_binding(target,en,"enumerator",ty,-1,-1,e.location);
      Binding &b=scopes_[target].bindings[bid]; b.value=value; b.has_value=true;
      b.enum_id=scoped?enum_scope+1:0;
      if(qualified && scoped) {
        b.display_name_id=intern_name(full_name);
        b.display_kind=DISPLAY_ENUM_CLASS;
      }
      if(value==LLONG_MAX && i+1<node.children.size())throw std::runtime_error("enumerator overflow");
      next=value+1;
    }
    return ty;
  }
  bool contains_initializer(const Node &item,const Node **initializer) const {
    for(std::size_t i=0;i<item.children.size();++i)
      if(item.children[i].text=="initializer") { if(initializer)*initializer=&item.children[i]; return true; }
    return false;
  }
  const Node *initializer_value(const Node &init) const {
    if(init.children.empty())return 0;
    const Node &child=init.children[0];
    if(child.text=="paren-initializer") {
      return child.children.empty()?0:&child.children[0];
    }
    return &child;
  }
  int merge_array_types(int old_type,int new_type) {
    if(old_type<0||new_type<0)return old_type;
    const Type &a=types_[old_type], &b=types_[new_type];
    if(a.kind==TY_ARRAY && b.kind==TY_ARRAY && a.base==b.base && a.bound==0 && b.bound>0)
      return new_type;
    return old_type;
  }
  int find_redeclared_entity(int scope,const std::string &name,const std::string &kind,int type) {
    const int name_id=find_name_id(name);
    if(name_id<0)return -1;
    const BindingIndices *f=scopes_[scope].by_name.find(static_cast<std::uint32_t>(name_id));
    if(!f)return -1;
    for(std::size_t i=0;i<f->size();++i) {
      const Binding &old=scopes_[scope].bindings[(*f)[i]];
      if(old.kind!=kind)continue;
      if(kind=="function") {
        if(type<0 || types_[type].kind!=TY_FUNCTION || old.function_identity!=types_[type].function_identity)continue;
        if(old.type<0 || types_[old.type].base!=types_[type].base)
          throw std::runtime_error("incompatible function return type");
        return old.entity_id;
      }
      if(kind=="variable") {
        if(old.type!=type)throw std::runtime_error("incompatible object redeclaration");
        return old.entity_id;
      }
    }
    return -1;
  }
  void complete_prior_arrays(int scope,const std::string &name,int type) {
    int owner=scope; std::string simple=name;
    if(name.find("::")!=std::string::npos) owner=scope_for_qualified_name(scope,name,&simple);
    int name_id=find_name_id(simple);
    if(name_id<0)return;
    const BindingIndices *f=scopes_[owner].by_name.find(static_cast<std::uint32_t>(name_id));
    if(!f)return;
    for(std::size_t i=0;i<f->size();++i) {
      Binding &b=scopes_[owner].bindings[(*f)[i]];
      if(b.kind=="variable") {
        b.type=merge_array_types(b.type,type);
        if(b.entity_id>=0)entities_[b.entity_id].canonical_type=b.type;
      }
    }
  }
  bool is_typedef(const Node &spec) const { return has_spec(spec,"typedef"); }
  bool is_extern(const Node &spec) const { return has_spec(spec,"extern"); }
  int declaration_target_scope(int scope,const std::string &name,std::string *simple) {
    if(name.find("::")==std::string::npos) {if(simple)*simple=name;return scope;}
    return scope_for_qualified_name(scope,name,simple);
  }
  void process_simple(const Node &node,int scope) {
    const Node *spec=0,*list=0;
    for(std::size_t i=0;i<node.children.size();++i) {
      if(node.children[i].text=="decl-specifier-seq")spec=&node.children[i];
      if(node.children[i].text=="init-declarator-list")list=&node.children[i];
    }
    if(!spec)return;
    for(std::size_t i=0;i<spec->children.size();++i) {
      const Node &sp=spec->children[i];
      if(sp.text.compare(0,15,"class-specifier")==0 && sp.text=="class-specifier" &&
         class_key(sp)=="union" && scopes_[scope].kind=="namespace" && !has_spec(*spec,"static"))
        throw std::runtime_error("namespace-scope anonymous union requires static");
    }
    std::string inferred;
    if(list&&!list->children.empty()) inferred=declarator_name(list->children[0].children[0]);
    bool class_or_enum=false;
    for(std::size_t i=0;i<spec->children.size();++i)
      if(spec->children[i].text.compare(0,15,"class-specifier")==0 ||
         spec->children[i].text.compare(0,14,"enum-specifier")==0)class_or_enum=true;
    int base=type_from_specifiers(*spec,scope,class_or_enum?tail_name(inferred):"");
    if(!list)return;
    bool td=is_typedef(*spec), ext=is_extern(*spec);
    for(std::size_t i=0;i<list->children.size();++i) {
      const Node &item=list->children[i];
      const Node *decl=item.children.empty()?0:&item.children[0];
      if(!decl)continue;
      std::string original_name;
      int ty=derive_declarator(*decl,base,scope,&original_name);
      if(has_spec(*spec,"constexpr") && ty>=0 && types_[ty].kind!=TY_FUNCTION) ty=cv_type(ty,true,false);
      if(original_name.empty())throw std::runtime_error("declaration without name");
      std::string name=original_name;
      int target=declaration_target_scope(scope,name,&name);
      if(!is_ancestor_scope(scope,target))throw std::runtime_error("qualified definition in non-enclosing scope");
      const Node *init=0; bool initialized=contains_initializer(item,&init);
      if(!td && types_[ty].kind==TY_ARRAY && types_[ty].bound==0 && init &&
         !init->children.empty() && init->children[0].text=="braced-init-list") {
        const std::size_t count=init->children[0].children.size();
        if(count)ty=array_type(types_[ty].base,static_cast<unsigned long long>(count));
      }
      if(!td && types_[ty].kind==TY_ARRAY && types_[ty].bound==0) {
        int name_id=find_name_id(name);
        const BindingIndices *prior=name_id<0?0:scopes_[target].by_name.find(static_cast<std::uint32_t>(name_id));
        if(prior) for(std::size_t q=0;q<prior->size();++q) {
          const Binding &old=scopes_[target].bindings[(*prior)[q]];
          if(old.kind=="variable" && old.type>=0 && types_[old.type].kind==TY_ARRAY &&
             types_[old.type].bound>0) { ty=old.type; break; }
        }
      }
      if(!td && is_void_type(ty))
        throw std::runtime_error("object has void type");
      if(!td && (types_[ty].kind==TY_LREF || types_[ty].kind==TY_RREF) &&
         !initialized && !ext && scopes_[target].kind!="class")
        throw std::runtime_error("reference requires initializer");
      if(td) add_binding(target,name,"type-alias",ty,-1,-1,item.location);
      else if(types_[ty].kind==TY_FUNCTION) {
        int old_entity=find_redeclared_entity(target,name,"function",ty);
        add_binding(target,name,"function",ty,-1,old_entity,item.location);
      } else {
        if(types_[ty].kind==TY_ARRAY)complete_prior_arrays(target,name,ty);
        int old_entity=find_redeclared_entity(target,name,"variable",ty);
        int id=add_binding(target,name,"variable",ty,-1,old_entity,item.location);
        Binding &b=scopes_[target].bindings[id];
        b.is_static=has_spec(*spec,"static");
        if(initialized && init) {
          const Node *value=initializer_value(*init);
          if(value && value->text=="braced-init-list" && value->children.size()==1)
            value=&value->children[0];
          if(value) {
            const int value_enum=expression_scoped_enum_id(*value,scope);
            const int target_enum=scoped_enum_id_of_type(ty);
            if((value_enum || target_enum) && value_enum!=target_enum)
              throw std::runtime_error("invalid implicit scoped-enum conversion");
            ConstValue c;
            try { c=eval(*value,scope); }
            catch(const std::exception &) {
              // A constexpr declaration requires a constant initializer;
              // ordinary const objects may have non-constant initialization.
              if(has_spec(*spec,"constexpr"))throw;
            }
            if(has_spec(*spec,"constexpr") && !c.valid)
              throw std::runtime_error("constexpr initializer is not a constant expression");
            if(c.valid && const_integral_value_type(ty) &&
               (const_qualified_value_type(ty) || has_spec(*spec,"constexpr"))) {
              b.value=c.value; b.has_value=true; b.enum_id=c.enum_id;
            }
          }
        }
        if(types_[ty].kind==TY_ARRAY) complete_prior_arrays(target,name,ty);
      }
    }
  }
  void process_alias(const Node &node,int scope) {
    std::string name=after_prefix(node.text,"alias-declaration ");
    if(name.empty())throw std::runtime_error("invalid alias declaration");
    if(node.children.empty())throw std::runtime_error("alias type expected");
    int ty=type_id_type(node.children[0],scope);
    add_binding(scope,name,"type-alias",ty,-1,-1,node.location);
  }
  void process_using_declaration(const Node &node,int scope) {
    if(node.children.empty())throw std::runtime_error("using target expected");
    std::string name=after_prefix(node.children[0].text,"target ");
    if(name.find('<')!=std::string::npos)throw std::runtime_error("using declaration names template-id");
    std::vector<std::string> parts=split_qualified(name);
    if(parts.empty())throw std::runtime_error("invalid using target");
    std::string tail=parts.back(); parts.pop_back();
    int target=scope;
    if(!parts.empty()) {
      std::string owner;
      for(std::size_t i=0;i<parts.size();++i){if(i)owner+="::";owner+=parts[i];}
      target=scope_for_qualified_name(scope,owner+"::__pa6_target");
    } else if(name.compare(0,2,"::")==0) target=root_scope(scope);
    int from=latest_binding(target,tail);
    if(from<0) {
      std::pair<int,int> f=lookup_direct_or_directive(target,tail);
      if(f.first>=0){target=f.first;from=f.second;}
    }
    if(from<0)throw std::runtime_error("using declaration target not found");
    const Binding source=binding(target,from);
    if(source.kind=="namespace" || source.kind=="namespace-alias")throw std::runtime_error("using target is not declaration");
    add_binding(scope,tail,source.kind,source.type,source.target_scope,
                source.entity_id,node.location);
    int imported=static_cast<int>(scopes_[scope].bindings.size()-1);
    scopes_[scope].bindings[imported].value=source.value;
    scopes_[scope].bindings[imported].has_value=source.has_value;
    scopes_[scope].bindings[imported].enum_id=source.enum_id;
    scopes_[scope].bindings[imported].function_identity=source.function_identity;
    scopes_[scope].bindings[imported].entity_id=source.entity_id;
  }
  void process_namespace(const Node &node,int scope) {
    std::string text=after_prefix(node.text,"namespace-definition");
    text=trim(text); if(text.empty())text="<unnamed>";
    bool inl=false;
    for(std::size_t i=0;i<node.children.size();++i)if(node.children[i].text=="inline")inl=true;
    int child=find_child_scope(scope,"namespace",text);
    if(child<0) {
      int old=latest_binding(scope,text);
      if(old>=0 && binding(scope,old).kind=="namespace-alias")throw std::runtime_error("cannot reopen namespace alias");
      child=add_scope("namespace",text,scope,true,node.location);
      scopes_[child].inline_namespace=inl;
      scopes_[child].entity_id=add_entity(scope,intern_name(text),"namespace",-1,-1,node.location);
      add_namespace_binding(scope,text,child,"namespace",node.location);
    } else scopes_[child].inline_namespace=scopes_[child].inline_namespace||inl;
    for(std::size_t i=0;i<node.children.size();++i)
      if(node.children[i].text!="inline")process_declaration(node.children[i],child);
  }
  void process_namespace_alias(const Node &node,int scope) {
    std::string name=after_prefix(node.text,"namespace-alias-definition ");
    if(node.children.empty())throw std::runtime_error("namespace alias target expected");
    std::string target_name=after_prefix(node.children[0].text,"target ");
    int target=require_namespace(scope,target_name);
    if(target<0)throw std::runtime_error("namespace alias target invalid");
    int old=latest_binding(scope,name);
    if(old>=0) {
      const Binding &b=binding(scope,old);
      if(b.kind=="namespace-alias" && b.target_scope==target)return;
      throw std::runtime_error("namespace alias conflicts with binding");
    }
    add_binding(scope,name,"namespace-alias",-1,target,-1,node.location);
  }
  void process_using_directive(const Node &node,int scope) {
    if(node.children.empty())return;
    std::string name=after_prefix(node.children[0].text,"target ");
    int target=require_namespace(scope,name);
    if(target<0)throw std::runtime_error("using directive target invalid");
    scopes_[scope].directives.push_back(target);
  }
  void process_static_assert(const Node &node,int scope) {
    if(node.children.empty())throw std::runtime_error("static_assert expression missing");
    ConstValue v=eval(node.children[0],scope);
    if(v.enum_id)throw std::runtime_error("scoped enum is not contextually convertible to bool");
    if(!v.valid || !v.value)throw std::runtime_error("static_assert failed");
  }
  int process_function(const Node &node,int scope,bool defer_body=false) {
    const Node *spec=0,*decl=0,*body=0;
    for(std::size_t i=0;i<node.children.size();++i) {
      if(node.children[i].text=="decl-specifier-seq")spec=&node.children[i];
      else if(node.children[i].text=="declarator")decl=&node.children[i];
      else if(node.children[i].text=="compound-statement" || node.children[i].text=="function-try-block")body=&node.children[i];
    }
    if(!spec || !decl)throw std::runtime_error("invalid function definition");
    std::string name;
    int target=scope;
    // A qualified member definition is owned by its target scope, not by the
    // lexical scope in which its spelling occurs.
    name=declarator_name(*decl);
    if(name.empty())throw std::runtime_error("function has no name");
    if(name.find("::")!=std::string::npos) {
      std::string simple; target=declaration_target_scope(scope,name,&simple); name=simple;
      if(!is_ancestor_scope(scope,target))throw std::runtime_error("qualified function definition in non-enclosing scope");
    }
    int base=type_from_specifiers(*spec,target);
    int type=derive_declarator(*decl,base,target);
    while(type>=0 && types_[type].kind==TY_CV)type=types_[type].base;
    if(type<0 || types_[type].kind!=TY_FUNCTION)throw std::runtime_error("function definition declarator is not a function");
    int old_entity=find_redeclared_entity(target,name,"function",type);
    int function_binding=add_binding(target,name,"function",type,-1,old_entity,node.location);
    int fn_scope=add_scope("function",name,target,true,node.location);
    scopes_[fn_scope].entity_id=scopes_[target].bindings[function_binding].entity_id;
    for(std::size_t i=0;i<decl->children.size();++i) {
      const Node &clause=decl->children[i]; if(clause.text!="parameter-clause")continue;
      for(std::size_t j=0;j<clause.children.size();++j) {
        const Node &param=clause.children[j]; if(param.text!="parameter-declaration")continue;
        std::string pname;
        const Node *pdecl=0;
        for(std::size_t k=0;k<param.children.size();++k)
          if(param.children[k].text=="declarator" || param.children[k].text=="abstract-declarator")pdecl=&param.children[k];
        if(pdecl)pname=declarator_name(*pdecl);
        int pt=parameter_type(param,target);
        add_binding(fn_scope,pname,"parameter",pt,-1,-1,param.location);
      }
    }
    if(body) {
      if(defer_body) {
      } else process_function_body(node,fn_scope,target);
    }
    return fn_scope;
  }
  void process_function_body(const Node &function,int fn_scope,int owner) {
    const Node *body=0;
    for(std::size_t i=0;i<function.children.size();++i)
      if(function.children[i].text=="compound-statement" || function.children[i].text=="function-try-block")body=&function.children[i];
    if(!body)return;
    process_statement(*body,fn_scope,owner,true);
  }
  void process_substatement(const Node &node,int scope) {
    if(node.text=="then"||node.text=="else") {
      if(node.children.empty())return;
      if(node.children[0].text=="compound-statement") {
        process_statement(node,scope,scope,false);
      } else {
        int substatement_scope=add_scope("block","",scope,true,node.location);
        process_statement(node,substatement_scope,substatement_scope,false);
      }
      return;
    }
    if(node.text=="compound-statement") {
      process_statement(node,scope,scope,false);
      return;
    }
    int substatement_scope=add_scope("block","",scope,true,node.location);
    process_statement(node,substatement_scope,substatement_scope,false);
  }
  void process_statement(const Node &node,int scope,int owner,bool force_block=false) {
    if(node.text.compare(0,15,"call-expression")==0 && !node.children.empty() &&
       node.children[0].text.compare(0,14,"id-expression ")==0 &&
       (node.children.size()<2 || node.children[1].children.empty())) {
      const std::string callee=node.children[0].text.substr(14);
      std::pair<int,int> found=resolve_name(scope,callee,false,false);
      if(found.first>=0) {
        const Binding &b=binding(found.first,found.second);
        int ty=b.type;
        while(ty>=0 && types_[ty].kind==TY_CV)ty=types_[ty].base;
        if(b.kind=="type" || b.kind=="type-alias") {
          if(ty>=0 && types_[ty].kind==TY_CLASS) {
            int cls=types_[ty].owner_scope;
            if(find_child_scope(cls,"function",tail_name(callee))<0) {
              const std::string ctor=tail_name(callee);
              const int ctor_type=function_type(builtin("void"),std::vector<int>(),false);
              const int ctor_entity=add_entity(cls,intern_name(ctor),"function",ctor_type,
                                               types_[ctor_type].function_signature);
              const int ctor_scope=add_scope("function",ctor,cls);
              scopes_[ctor_scope].entity_id=ctor_entity;
            }
          }
        }
      }
    }
    if(retain_syntax_ && node.text=="condition-declaration") {
      const Node *spec=0,*decl=0;
      for(std::size_t i=0;i<node.children.size();++i) {
        if(node.children[i].text=="decl-specifier-seq")spec=&node.children[i];
        else if(node.children[i].text=="declarator")decl=&node.children[i];
      }
      if(spec && decl) {
        std::string name;
        int ty=derive_declarator(*decl,type_from_specifiers(*spec,scope),scope,&name);
        if(!name.empty())bind_value(scope,name,"variable",ty,node.location);
      }
    }
    if(node.text=="compound-statement") {
      int block=add_scope("block","",scope,true,node.location);
      for(std::size_t i=0;i<node.children.size();++i)
        process_statement(node.children[i],block,block,false);
      return;
    }
    // Selection and iteration statements own their condition/initializer
    // declarations. A for-init name must disappear after the loop; a
    // condition-declaration is shared by its substatements but not by the
    // containing compound statement.
    if(node.text=="if-statement" || node.text=="switch-statement" ||
       node.text=="while-statement" || node.text=="for-statement") {
      int statement_scope=add_scope("block","",scope,true,node.location);
      if(node.text=="if-statement") {
        if(!node.children.empty())process_statement(node.children[0],statement_scope,statement_scope,false);
        for(std::size_t i=1;i<node.children.size();++i)
          process_substatement(node.children[i],statement_scope);
      } else if(node.text=="while-statement"||node.text=="switch-statement") {
        if(!node.children.empty())process_statement(node.children[0],statement_scope,statement_scope,false);
        if(node.children.size()>1)process_substatement(node.children[1],statement_scope);
      } else {
        for(std::size_t i=0;i+1<node.children.size();++i)
          process_statement(node.children[i],statement_scope,statement_scope,false);
        if(!node.children.empty())process_substatement(node.children.back(),statement_scope);
      }
      return;
    }
    if(node.text=="do-statement") {
      if(!node.children.empty())process_substatement(node.children[0],scope);
      for(std::size_t i=1;i<node.children.size();++i)
        process_statement(node.children[i],scope,owner,false);
      return;
    }
    if(node.text=="function-try-block") {
      for(std::size_t i=0;i<node.children.size();++i) process_statement(node.children[i],scope,owner,false);
      return;
    }
    if(node.text=="simple-declaration" || node.text=="class-specifier" ||
       node.text=="enum-specifier" || node.text=="class-forward-declaration" ||
       node.text.compare(0,std::string("namespace-definition").size(),"namespace-definition")==0 ||
       node.text.compare(0,std::string("namespace-alias-definition").size(),"namespace-alias-definition")==0 ||
       node.text=="using-declaration" || node.text=="using-directive" ||
       node.text.compare(0,std::string("alias-declaration").size(),"alias-declaration")==0 || node.text=="static-assert-declaration" ||
       node.text=="function-definition" || node.text=="template-declaration") {
      process_declaration(node,scope); return;
    }
    for(std::size_t i=0;i<node.children.size();++i) {
      const Node &ch=node.children[i];
      if(ch.text=="compound-statement")process_statement(ch,scope,owner,true);
      else if(ch.text=="simple-declaration" || ch.text=="class-specifier" || ch.text=="enum-specifier" ||
              ch.text=="class-forward-declaration" || ch.text=="static-assert-declaration")
        process_declaration(ch,scope);
      else process_statement(ch,scope,owner,false);
    }
  }
  void process_template_parameter(const Node &node,int scope) {
    if(node.text=="type-parameter") {
      std::string name;
      bool template_template=false;
      for(std::size_t i=0;i<node.children.size();++i) {
        const Node &ch=node.children[i];
        if(ch.text.compare(0,11,"identifier ")==0) name=ch.text.substr(11);
        if(ch.text=="template-template-parameter")template_template=true;
      }
      if(name.empty()) return;
      Type t; t.kind=TY_BUILTIN; t.owner_scope=scope;
      t.template_template=template_template;
      t.name_id=intern_name(template_template?"template-parameter "+name:"typename "+name);
      int ty=new_type(t);
      add_binding(scope,name,"type",ty,-1,-1,node.location);
      return;
    }
    if(node.text=="non-type-template-parameter") {
      std::string name;
      for(std::size_t i=0;i<node.children.size();++i)
        if(node.children[i].text=="declarator")name=declarator_name(node.children[i]);
      if(!name.empty()) {
        int t=-1;
        for(std::size_t i=0;i<node.children.size();++i)
          if(node.children[i].text=="decl-specifier-seq")t=type_from_specifiers(node.children[i],scope);
        bind_value(scope,name,"variable",t,node.location);
      }
    }
  }
  void process_template(const Node &node,int scope) {
    int params=add_scope("template-parameters","",scope,true,node.location);
    if(node.children.empty())return;
    const Node &clause=node.children[0];
    for(std::size_t i=0;i<clause.children.size();++i) {
      const Node &child=clause.children[i];
      if(child.text=="template-parameter-list") {
        for(std::size_t j=0;j<child.children.size();++j)process_template_parameter(child.children[j],params);
      }
    }
    // The inner template-template parameter names are deliberately confined to
    // their parameter declaration and never leak into the enclosing template.
    if(node.children.size()>1)process_declaration(node.children[1],params);
  }
  void process_special_member(const Node &node,int scope) {
    std::string name=after_prefix(node.text,"special-member-definition ");
    const Node *decl=0,*body=0;
    for(std::size_t i=0;i<node.children.size();++i) {
      if(node.children[i].text=="declarator")decl=&node.children[i];
      if(node.children[i].text=="compound-statement")body=&node.children[i];
    }
    if(!decl) return;
    int target=scope;std::string simple=tail_name(name);
    if(name.find("::")!=std::string::npos)target=scope_for_qualified_name(scope,name,&simple);
    if(!is_ancestor_scope(scope,target))throw std::runtime_error("qualified special member definition in non-enclosing scope");
    std::vector<int> params;
    for(std::size_t i=0;i<decl->children.size();++i)if(decl->children[i].text=="parameter-clause")
      for(std::size_t j=0;j<decl->children[i].children.size();++j)
        if(decl->children[i].children[j].text=="parameter-declaration")params.push_back(parameter_type(decl->children[i].children[j],target));
    int constructor_type=function_type(builtin("void"),params,false);
    int old_entity=find_redeclared_entity(target,simple,"function",constructor_type);
    int fn=add_binding(target,simple,"function",constructor_type,-1,old_entity,node.location);
    int fn_scope=add_scope("function",simple,target,true,node.location);
    scopes_[fn_scope].entity_id=scopes_[target].bindings[fn].entity_id;
    for(std::size_t i=0;i<decl->children.size();++i)if(decl->children[i].text=="parameter-clause")
      for(std::size_t j=0;j<decl->children[i].children.size();++j) {
        const Node &p=decl->children[i].children[j];if(p.text!="parameter-declaration")continue;
        const Node *pd=0;for(std::size_t k=0;k<p.children.size();++k)if(p.children[k].text=="declarator"||p.children[k].text=="abstract-declarator")pd=&p.children[k];
        bind_value(fn_scope,pd?declarator_name(*pd):"","parameter",
                   parameter_type(p,target),p.location);
      }
    if(body)process_statement(*body,fn_scope,target,true);
  }
  void process_declaration(const Node &node,int scope) {
    const std::string &text=node.text;
    if(text=="translation-unit") {
      for(std::size_t i=0;i<node.children.size();++i)process_declaration(node.children[i],scope);
    } else if(text.compare(0,20,"namespace-definition")==0) process_namespace(node,scope);
    else if(text.compare(0,26,"namespace-alias-definition")==0)process_namespace_alias(node,scope);
    else if(text=="using-directive")process_using_directive(node,scope);
    else if(text=="using-declaration")process_using_declaration(node,scope);
    else if(text.compare(0,17,"alias-declaration")==0)process_alias(node,scope);
    else if(text=="template-declaration")process_template(node,scope);
    else if(text=="simple-declaration")process_simple(node,scope);
    else if(text=="function-definition")process_function(node,scope,false);
    else if(text.compare(0,15,"class-specifier")==0 || text.compare(0,25,"class-forward-declaration")==0)analyze_class(node,scope);
    else if(text.compare(0,14,"enum-specifier")==0)analyze_enum(node,scope);
    else if(text=="static-assert-declaration")process_static_assert(node,scope);
    else if(text.compare(0,25,"special-member-definition")==0)process_special_member(node,scope);
    else if(text=="linkage-specification" || text=="linkage-declaration")
      for(std::size_t i=0;i<node.children.size();++i)process_declaration(node.children[i],scope);
    else if(text=="empty-declaration" || text=="access-specifier" || text=="base-clause" || text=="class-key") {}
    else if(text=="compound-statement")process_statement(node,scope,scope,true);
    else if(text=="simple-declaration" || text=="static_assert-declaration") {}
    else if(text.compare(0,14,"explicit-instantiation")==0 || text=="explicit-specialization") {}
    else {
      // Template and declaration wrappers occasionally occur in grammar nodes;
      // visit only recognized declarations, never parse their rendered text.
      for(std::size_t i=0;i<node.children.size();++i) {
        const Node &c=node.children[i];
        if(c.text=="simple-declaration" || c.text=="function-definition" || c.text=="class-specifier" || c.text=="class-forward-declaration" || c.text=="enum-specifier") process_declaration(c,scope);
      }
    }
  }
  void release_syntax_tree() {
    typedef std::vector<Node,SyntaxArenaAllocator<Node> > NodeVector;
    {
      NodeVector discarded((SyntaxArenaAllocator<Node>(tree_.arena.get())));
      tree_.root.children.swap(discarded);
    }
    source_files_.swap(tree_.source_files);
    tree_.arena.reset();
  }
  void render_scope(int id,std::ostream &out,unsigned depth) const {
    const Scope &s=scopes_[id];
    if(!s.visible)return;
    for(unsigned i=0;i<depth;++i)out<<"  ";
    out<<"scope "<<s.kind;
    if(s.name_id!=UINT32_MAX && !name_text(s.name_id).empty())
      out<<" "<<name_text(s.name_id);
    out<<'\n';
    for(std::size_t i=0;i<s.bindings.size();++i) {
      std::string line=output_binding(s.bindings[i]);
      if(line.empty())continue;
      for(unsigned j=0;j<depth+1;++j)out<<"  ";
      out<<line<<'\n';
    }
    if(s.kind=="class") {
      for(std::size_t i=0;i<s.children.size();++i)
        if(scopes_[s.children[i]].kind!="function")render_scope(s.children[i],out,depth+1);
      for(std::size_t i=0;i<s.children.size();++i)
        if(scopes_[s.children[i]].kind=="function")render_scope(s.children[i],out,depth+1);
    } else {
      for(std::size_t i=0;i<s.children.size();++i)render_scope(s.children[i],out,depth+1);
    }
  }
public:
  explicit Analyzer(SyntaxTree &tree, bool retain_syntax) : tree_(tree),lookup_generation_(0),global_(-1),allow_anon_union_(false),anonymous_union_serial_(0),retain_syntax_(retain_syntax) {
    source_line_starts_.resize(tree.source_files.size());
    builtin("void"); builtin("bool"); builtin("char"); builtin("signed char"); builtin("unsigned char");
    builtin("char16_t");builtin("char32_t");builtin("wchar_t");builtin("short int");builtin("unsigned short int");
    builtin("int");builtin("unsigned int");builtin("long int");builtin("unsigned long int");builtin("long long int");
    builtin("unsigned long long int");builtin("float");builtin("double");builtin("long double");builtin("nullptr_t");
    global_=add_scope("namespace","<global>",-1);
  }
  void build() {
    process_declaration(tree_.root,global_);
    if (retain_syntax_) source_files_ = tree_.source_files;
    else release_syntax_tree();
  }
  void write_dump(std::ostream &out) const {
    out<<"translation-unit\n";
    render_scope(global_,out,1);
  }
  std::size_t type_count() const { return types_.size(); }
  std::size_t scope_count() const { return scopes_.size(); }
  std::size_t entity_count() const { return entities_.size(); }
  std::size_t signature_count() const { return function_signatures_.size(); }
  std::size_t identity_count() const { return function_identities_.size(); }
  std::size_t source_file_count() const { return source_files_.size(); }
  SemanticNameId find_name_by_text(const std::string &text) const {
    const int id=find_name_id(text);
    return id<0?UINT32_MAX:static_cast<SemanticNameId>(id);
  }
  const std::string &name_by_id(SemanticNameId id) const { return name_text(id); }
  const PostTokenSourceFile &source_file(std::size_t id) const { return source_files_.at(id); }
  std::pair<int,int> lookup_unqualified_id(int scope,SemanticNameId name,
                                           bool type_only,bool namespace_only) const {
    return unqualified_lookup(scope,name,type_only,namespace_only);
  }
  std::pair<int,int> lookup_qualified_ids(int scope,
                                          const std::vector<SemanticNameId> &path,
                                          bool absolute,bool type_only,
                                          bool namespace_only) const {
    return qualified_lookup_ids(scope,path,absolute,type_only,namespace_only);
  }
  SemanticTypeInfo type_info(int id) const {
    if(id<0 || id>=static_cast<int>(types_.size()))throw std::out_of_range("invalid semantic type ID");
    const Type &t=types_[id]; SemanticTypeInfo out;
    out.kind=static_cast<SemanticTypeKind>(t.kind); out.base=t.base; out.array_bound=t.bound;
    out.variadic=t.variadic; out.is_const=t.is_const; out.is_volatile=t.is_volatile;
    out.scoped=t.scoped; out.template_template=t.template_template;
    out.owner_scope=t.owner_scope; out.enum_underlying_type=-1;
    if(t.kind==TY_ENUM) {
      const int *underlying=enum_underlying_.find(t.owner_scope);
      if(underlying)out.enum_underlying_type=*underlying;
    }
    out.function_signature=t.function_signature;
    out.function_identity=t.function_identity; out.name=t.name_id;
    out.class_tag=t.tag=="class"?1:t.tag=="struct"?2:t.tag=="union"?3:0;
    out.parameter_count=t.params.size();
    return out;
  }
  int type_from_syntax_node(const Node &node,int scope) {
    return type_id_type(node,scope);
  }
  int type_parameter(int id,std::size_t index) const {
    if(id<0 || id>=static_cast<int>(types_.size()) || index>=types_[id].params.size())
      throw std::out_of_range("invalid function type parameter index");
    return types_[id].params[index];
  }
  int signature_parameter(int id,std::size_t index) const {
    if(id<0 || id>=static_cast<int>(function_signatures_.size()) ||
       index>=function_signatures_[id].parameters.size())
      throw std::out_of_range("invalid function signature parameter index");
    return function_signatures_[id].parameters[index];
  }
  int identity_parameter(int id,std::size_t index) const {
    if(id<0 || id>=static_cast<int>(function_identities_.size()) ||
       index>=function_identities_[id].parameters.size())
      throw std::out_of_range("invalid function identity parameter index");
    return function_identities_[id].parameters[index];
  }
  SemanticScopeInfo scope_info(int id) const {
    if(id<0 || id>=static_cast<int>(scopes_.size()))throw std::out_of_range("invalid semantic scope ID");
    const Scope &s=scopes_[id]; SemanticScopeInfo out;
    out.kind=static_cast<SemanticScopeKind>(scope_kind_code(s.kind));
    out.name=s.name_id; out.parent=s.parent;
    out.entity=s.entity_id; out.visible=s.visible; out.inline_namespace=s.inline_namespace;
    out.union_class=s.union_class; out.location=s.location;
    out.binding_count=s.bindings.size(); out.child_count=s.children.size();
    out.directive_count=s.directives.size(); out.base_count=s.base_types.size();
    out.object_size=s.object_size; out.object_alignment=s.object_alignment;
    out.layout_complete=s.layout_complete; out.layout_computed=s.layout_computed;
    out.layout_unsupported=s.layout_unsupported;
    out.layout_empty=s.layout_empty; out.has_vptr=s.layout_has_vptr;
    return out;
  }
  std::vector<SemanticBindingInfo> bindings_named_info(int scope,SemanticNameId name) const {
    std::vector<SemanticBindingInfo> out;
    if(scope<0||scope>=static_cast<int>(scopes_.size()))return out;
    const BindingIndices *indices=scopes_[scope].by_name.find(name);
    if(!indices)return out;
    out.reserve(indices->size());
    for(std::size_t i=0;i<indices->size();++i)out.push_back(binding_info(scope,(*indices)[i]));
    return out;
  }
  std::vector<int> binding_indices_named(int scope,SemanticNameId name) const {
    std::vector<int> out;
    if(scope<0||scope>=static_cast<int>(scopes_.size()))return out;
    const BindingIndices *indices=scopes_[scope].by_name.find(name);
    if(!indices)return out;
    out.reserve(indices->size());
    for(std::size_t i=0;i<indices->size();++i)out.push_back((*indices)[i]);
    return out;
  }
  SemanticLookupResult binding_at(int scope,SemanticNameId name,
                                  const SyntaxLocation &location) const {
    SemanticLookupResult out;
    if(scope<0||scope>=static_cast<int>(scopes_.size()))return out;
    const BindingIndices *indices=scopes_[scope].by_name.find(name);
    if(!indices||indices->empty())return out;
    int fallback=(*indices)[0];
    for(std::size_t i=0;i<indices->size();++i) {
      const int index=(*indices)[i];
      const Binding &b=scopes_[scope].bindings[index];
      if(location.valid()&&b.location.valid()&&b.location.packed==location.packed) {
        out.scope=scope;out.binding_index=index;return out;
      }
    }
    out.scope=scope;out.binding_index=fallback;return out;
  }
  SemanticBindingInfo binding_info(int scope,int index) const {
    if(scope<0 || scope>=static_cast<int>(scopes_.size()) || index<0 ||
       index>=static_cast<int>(scopes_[scope].bindings.size()))
      throw std::out_of_range("invalid semantic binding index");
    const Binding &b=scopes_[scope].bindings[index]; SemanticBindingInfo out;
    out.kind=static_cast<SemanticBindingKind>(
        b.kind=="type"?1:b.kind=="type-alias"?2:b.kind=="enumerator"?3:
        b.kind=="function"?4:b.kind=="variable"?5:b.kind=="parameter"?6:
        b.kind=="namespace"?7:b.kind=="namespace-alias"?8:0);
    out.name=b.name_id; out.type=b.type; out.target_scope=b.target_scope;
    out.entity=b.entity_id; out.function_identity=b.function_identity;
    out.constant_value=b.value; out.has_constant=b.has_value; out.is_static=b.is_static;
    out.enum_identity=b.enum_id;
    out.display_kind=static_cast<SemanticDisplayTypeKind>(b.display_kind);
    out.display_name=b.display_name_id; out.location=b.location;
    return out;
  }
  SemanticEntityInfo entity_info(int id) const {
    if(id<0 || id>=static_cast<int>(entities_.size()))throw std::out_of_range("invalid semantic entity ID");
    const Entity &e=entities_[id]; SemanticEntityInfo out;
    out.kind=static_cast<SemanticEntityKind>(
        e.kind=="namespace"?1:e.kind=="class"?2:e.kind=="enum"?3:
        e.kind=="function"?4:e.kind=="variable"?5:e.kind=="type"?6:0);
    out.name=e.name_id; out.owner_scope=e.owner_scope; out.canonical_type=e.canonical_type;
    out.function_signature=e.function_signature; out.first_location=e.first_location;
    return out;
  }
  SemanticFunctionSignatureInfo signature_info(int id) const {
    if(id<0 || id>=static_cast<int>(function_signatures_.size()))throw std::out_of_range("invalid semantic signature ID");
    const FunctionSignature &sig=function_signatures_[id]; SemanticFunctionSignatureInfo out;
    out.return_type=sig.return_type; out.variadic=sig.variadic;
    out.parameter_count=sig.parameters.size();
    return out;
  }
  SemanticFunctionIdentityInfo identity_info(int id) const {
    if(id<0 || id>=static_cast<int>(function_identities_.size()))throw std::out_of_range("invalid function identity ID");
    const FunctionIdentity &identity=function_identities_[id]; SemanticFunctionIdentityInfo out;
    out.variadic=identity.variadic;
    out.parameter_count=identity.parameters.size();
    return out;
  }
  int scope_child(int id,std::size_t index) const {
    if(id<0 || id>=static_cast<int>(scopes_.size()) || index>=scopes_[id].children.size())
      throw std::out_of_range("invalid semantic child-scope index");
    return scopes_[id].children[index];
  }
  int scope_directive(int id,std::size_t index) const {
    if(id<0 || id>=static_cast<int>(scopes_.size()) || index>=scopes_[id].directives.size())
      throw std::out_of_range("invalid namespace directive index");
    return scopes_[id].directives[index];
  }
  int scope_base(int id,std::size_t index) const {
    if(id<0 || id>=static_cast<int>(scopes_.size()) || index>=scopes_[id].base_types.size())
      throw std::out_of_range("invalid base-class index");
    return scopes_[id].base_types[index];
  }
};
} // namespace

class SemanticModel::Impl {
public:
  SyntaxTree tree;
  Analyzer analyzer;
  explicit Impl(SyntaxTree &&input, bool retain_syntax) : tree(std::move(input)),analyzer(tree, retain_syntax) {
    analyzer.build();
  }
};

SemanticModel::SemanticModel(SyntaxTree &&tree, bool retain_syntax) : impl_(new Impl(std::move(tree), retain_syntax)) {}
SemanticModel::~SemanticModel() {}
std::unique_ptr<SemanticModel> build_semantic_model(SyntaxTree &&tree, bool retain_syntax) {
  return std::unique_ptr<SemanticModel>(new SemanticModel(std::move(tree), retain_syntax));
}
const SyntaxNode &SemanticModel::syntax_root() const { return impl_->tree.root; }
std::size_t SemanticModel::type_count() const { return impl_->analyzer.type_count(); }
std::size_t SemanticModel::source_file_count() const { return impl_->analyzer.source_file_count(); }
std::size_t SemanticModel::scope_count() const { return impl_->analyzer.scope_count(); }
std::size_t SemanticModel::entity_count() const { return impl_->analyzer.entity_count(); }
std::size_t SemanticModel::function_signature_count() const { return impl_->analyzer.signature_count(); }
std::size_t SemanticModel::function_identity_count() const { return impl_->analyzer.identity_count(); }
SemanticNameId SemanticModel::find_name(const std::string &name) const { return impl_->analyzer.find_name_by_text(name); }
const std::string &SemanticModel::name_text(SemanticNameId name) const { return impl_->analyzer.name_by_id(name); }
const PostTokenSourceFile &SemanticModel::source_file(std::size_t file_id) const { return impl_->analyzer.source_file(file_id); }
SemanticTypeInfo SemanticModel::type(SemanticTypeId id) const { return impl_->analyzer.type_info(id); }
SemanticTypeId SemanticModel::type_from_syntax(const SyntaxNode &node,SemanticScopeId scope) {
  return impl_->analyzer.type_from_syntax_node(node,scope);
}
SemanticTypeId SemanticModel::type_parameter(SemanticTypeId id,std::size_t index) const {
  return impl_->analyzer.type_parameter(id,index);
}
SemanticScopeInfo SemanticModel::scope(SemanticScopeId id) const { return impl_->analyzer.scope_info(id); }
SemanticBindingInfo SemanticModel::binding(SemanticScopeId scope,std::size_t index) const {
  if(index>static_cast<std::size_t>(INT_MAX))throw std::out_of_range("invalid semantic binding index");
  return impl_->analyzer.binding_info(scope,static_cast<int>(index));
}
std::vector<SemanticBindingInfo> SemanticModel::bindings_named(SemanticScopeId scope,SemanticNameId name) const {
  return impl_->analyzer.bindings_named_info(scope,name);
}
std::vector<int> SemanticModel::binding_indices_named(SemanticScopeId scope,SemanticNameId name) const {
  return impl_->analyzer.binding_indices_named(scope,name);
}
SemanticLookupResult SemanticModel::binding_at(SemanticScopeId scope,SemanticNameId name,
                                               const SyntaxLocation &location) const {
  return impl_->analyzer.binding_at(scope,name,location);
}
SemanticEntityInfo SemanticModel::entity(SemanticEntityId id) const { return impl_->analyzer.entity_info(id); }
SemanticFunctionSignatureInfo SemanticModel::function_signature(SemanticSignatureId id) const { return impl_->analyzer.signature_info(id); }
SemanticTypeId SemanticModel::signature_parameter(SemanticSignatureId id,std::size_t index) const {
  return impl_->analyzer.signature_parameter(id,index);
}
SemanticFunctionIdentityInfo SemanticModel::function_identity(SemanticFunctionIdentityId id) const { return impl_->analyzer.identity_info(id); }
SemanticTypeId SemanticModel::identity_parameter(SemanticFunctionIdentityId id,std::size_t index) const {
  return impl_->analyzer.identity_parameter(id,index);
}
SemanticScopeId SemanticModel::scope_child(SemanticScopeId id,std::size_t index) const { return impl_->analyzer.scope_child(id,index); }
SemanticScopeId SemanticModel::scope_directive(SemanticScopeId id,std::size_t index) const { return impl_->analyzer.scope_directive(id,index); }
SemanticTypeId SemanticModel::scope_base(SemanticScopeId id,std::size_t index) const { return impl_->analyzer.scope_base(id,index); }
SemanticLookupResult SemanticModel::lookup_unqualified(SemanticScopeId scope,SemanticNameId name,
                                                         bool type_only,bool namespace_only) const {
  const std::pair<int,int> result=impl_->analyzer.lookup_unqualified_id(scope,name,type_only,namespace_only);
  SemanticLookupResult out; out.scope=result.first; out.binding_index=result.second; return out;
}
SemanticLookupResult SemanticModel::lookup_qualified(SemanticScopeId scope,
                                                      const std::vector<SemanticNameId> &path,
                                                      bool absolute,bool type_only,
                                                      bool namespace_only) const {
  const std::pair<int,int> result=impl_->analyzer.lookup_qualified_ids(
      scope,path,absolute,type_only,namespace_only);
  SemanticLookupResult out; out.scope=result.first; out.binding_index=result.second; return out;
}
void write_semantic_types(const SemanticModel &model,std::ostream &output) {
  model.impl_->analyzer.write_dump(output);
}
