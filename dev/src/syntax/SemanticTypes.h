#pragma once
#include "syntax/Parser.h"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

// Stable translation-unit identities exposed to later semantic/lowering phases.
// IDs are indices owned by one SemanticModel; none are rendered spellings.
typedef std::uint32_t SemanticNameId;
typedef int SemanticTypeId;
typedef int SemanticScopeId;
typedef int SemanticEntityId;
typedef int SemanticSignatureId;
typedef int SemanticFunctionIdentityId;

enum SemanticTypeKind { SEMANTIC_BUILTIN=0, SEMANTIC_CLASS, SEMANTIC_ENUM,
                        SEMANTIC_CV, SEMANTIC_POINTER, SEMANTIC_LVALUE_REFERENCE,
                        SEMANTIC_RVALUE_REFERENCE, SEMANTIC_ARRAY, SEMANTIC_FUNCTION };
enum SemanticScopeKind { SEMANTIC_SCOPE_UNKNOWN=0, SEMANTIC_NAMESPACE=1,
                         SEMANTIC_CLASS_SCOPE=2, SEMANTIC_ENUM_SCOPE=3,
                         SEMANTIC_FUNCTION_SCOPE=4, SEMANTIC_TEMPLATE_PARAMETERS=5,
                         SEMANTIC_BLOCK_SCOPE=6 };
enum SemanticBindingKind { SEMANTIC_BINDING_UNKNOWN=0, SEMANTIC_TYPE=1,
                           SEMANTIC_TYPE_ALIAS=2, SEMANTIC_ENUMERATOR=3,
                           SEMANTIC_FUNCTION_BINDING=4, SEMANTIC_VARIABLE=5,
                           SEMANTIC_PARAMETER=6, SEMANTIC_NAMESPACE_BINDING=7,
                           SEMANTIC_NAMESPACE_ALIAS=8 };
enum SemanticDisplayTypeKind { SEMANTIC_DISPLAY_DEFAULT=0, SEMANTIC_DISPLAY_CLASS=1,
                               SEMANTIC_DISPLAY_STRUCT=2, SEMANTIC_DISPLAY_UNION=3,
                               SEMANTIC_DISPLAY_ENUM=4, SEMANTIC_DISPLAY_ENUM_CLASS=5 };
enum SemanticEntityKind { SEMANTIC_ENTITY_UNKNOWN=0, SEMANTIC_NAMESPACE_ENTITY=1,
                          SEMANTIC_CLASS_ENTITY=2, SEMANTIC_ENUM_ENTITY=3,
                          SEMANTIC_FUNCTION_ENTITY=4, SEMANTIC_VARIABLE_ENTITY=5,
                          SEMANTIC_TYPE_ENTITY=6 };

struct SemanticTypeInfo {
  SemanticTypeKind kind;
  SemanticTypeId base;
  std::size_t parameter_count;
  unsigned long long array_bound;
  bool variadic, is_const, is_volatile, scoped, template_template;
  SemanticScopeId owner_scope;
  SemanticTypeId enum_underlying_type;
  SemanticSignatureId function_signature;
  SemanticFunctionIdentityId function_identity;
  SemanticNameId name;
  int class_tag;
};
struct SemanticBindingInfo {
  SemanticBindingKind kind;
  SemanticNameId name;
  SemanticTypeId type;
  SemanticScopeId target_scope;
  SemanticEntityId entity;
  SemanticFunctionIdentityId function_identity;
  long long constant_value;
  bool has_constant, is_static;
  int enum_identity;
  SemanticDisplayTypeKind display_kind;
  SemanticNameId display_name;
  SyntaxLocation location;
};
struct SemanticScopeInfo {
  SemanticScopeKind kind;
  SemanticNameId name;
  SemanticScopeId parent;
  SemanticEntityId entity;
  bool visible, inline_namespace, union_class;
  SyntaxLocation location;
  std::size_t binding_count, child_count, directive_count, base_count;
  long long object_size, object_alignment;
  bool layout_complete, layout_computed, layout_unsupported, layout_empty, has_vptr;
};
struct SemanticEntityInfo {
  SemanticEntityKind kind;
  SemanticNameId name;
  SemanticScopeId owner_scope;
  SemanticTypeId canonical_type;
  SemanticSignatureId function_signature;
  SyntaxLocation first_location;
};
struct SemanticFunctionSignatureInfo {
  SemanticTypeId return_type;
  std::size_t parameter_count;
  bool variadic;
};
struct SemanticFunctionIdentityInfo {
  std::size_t parameter_count;
  bool variadic;
};
// scope/binding_index is (-1,-1) for a miss. Enum identity is encoded as
// owning enum-scope ID + 1; zero denotes an ordinary integral value.
struct SemanticLookupResult {
  SemanticScopeId scope;
  int binding_index;
  SemanticLookupResult() : scope(-1), binding_index(-1) {}
};

class SemanticModel {
  class Impl;
  std::unique_ptr<Impl> impl_;
  explicit SemanticModel(SyntaxTree &&tree);
  friend std::unique_ptr<SemanticModel> build_semantic_model(SyntaxTree &&tree);
  friend void write_semantic_types(const SemanticModel &model, std::ostream &output);
public:
  ~SemanticModel();
  SemanticModel(const SemanticModel &) = delete;
  SemanticModel &operator=(const SemanticModel &) = delete;

  std::size_t type_count() const;
  std::size_t source_file_count() const;
  std::size_t scope_count() const;
  std::size_t entity_count() const;
  std::size_t function_signature_count() const;
  std::size_t function_identity_count() const;
  SemanticNameId find_name(const std::string &name) const;
  const std::string &name_text(SemanticNameId name) const;
  const PostTokenSourceFile &source_file(std::size_t file_id) const;
  SemanticTypeInfo type(SemanticTypeId id) const;
  SemanticTypeId type_parameter(SemanticTypeId id, std::size_t index) const;
  SemanticScopeInfo scope(SemanticScopeId id) const;
  SemanticBindingInfo binding(SemanticScopeId scope, std::size_t index) const;
  SemanticEntityInfo entity(SemanticEntityId id) const;
  SemanticFunctionSignatureInfo function_signature(SemanticSignatureId id) const;
  SemanticTypeId signature_parameter(SemanticSignatureId id, std::size_t index) const;
  SemanticFunctionIdentityInfo function_identity(SemanticFunctionIdentityId id) const;
  SemanticTypeId identity_parameter(SemanticFunctionIdentityId id, std::size_t index) const;
  SemanticScopeId scope_child(SemanticScopeId id, std::size_t index) const;
  SemanticScopeId scope_directive(SemanticScopeId id, std::size_t index) const;
  SemanticTypeId scope_base(SemanticScopeId id, std::size_t index) const;
  SemanticLookupResult lookup_unqualified(SemanticScopeId scope,
                                          SemanticNameId name,
                                          bool type_only=false,
                                          bool namespace_only=false) const;
  SemanticLookupResult lookup_qualified(SemanticScopeId scope,
                                        const std::vector<SemanticNameId> &path,
                                        bool absolute=false,
                                        bool type_only=false,
                                        bool namespace_only=false) const;
};

// Parse once, transfer TU-owned source buffers and build a reusable typed graph.
// The structured syntax arena and post-token tape are released after analysis;
// source buffers and compact locations remain owned by the graph.
std::unique_ptr<SemanticModel> build_semantic_model(SyntaxTree &&tree);

// Deterministic PA6 text is a view over the same graph used by later modes.
void write_semantic_types(const SemanticModel &model, std::ostream &output);
