#include "syntax/SemanticTypes.h"
#include <cassert>

int main() {
  SyntaxTree tree;
  tree.arena.reset(new SyntaxArena());
  tree.root=SyntaxNode("translation-unit",tree.arena.get());
  const std::shared_ptr<const std::string> source(new std::string("int audited_value;"));
  PostTokenSourceFile source_file; source_file.path="api.cpp"; source_file.contents=source;
  tree.source_files.push_back(source_file);
  typedef std::vector<SyntaxNode,SyntaxArenaAllocator<SyntaxNode> > NodeVector;
  NodeVector &root=tree.root.children;
  root.emplace_back("simple-declaration",tree.arena.get());
  root.back().location.set(0,0);
  NodeVector &decl=root.back().children;
  decl.emplace_back("decl-specifier-seq",tree.arena.get());
  decl.back().children.emplace_back("decl-specifier KW_INT:int",tree.arena.get());
  decl.emplace_back("init-declarator-list",tree.arena.get());
  decl.back().children.emplace_back("init-declarator",tree.arena.get());
  decl.back().children.back().location.set(0,4);
  decl.back().children.back().children.emplace_back("declarator",tree.arena.get());
  decl.back().children.back().children.back().children.emplace_back(
      "identifier audited_value",tree.arena.get());

  std::unique_ptr<SemanticModel> model=build_semantic_model(std::move(tree));
  assert(model->scope_count()==1);
  assert(model->source_file_count()==1);
  assert(model->source_file(0).path=="api.cpp");
  assert(*model->source_file(0).contents=="int audited_value;");
  const SemanticScopeInfo global=model->scope(0);
  assert(global.kind==SEMANTIC_NAMESPACE && global.binding_count==1);
  const SemanticBindingInfo value=model->binding(0,0);
  assert(value.kind==SEMANTIC_VARIABLE && value.entity>=0 && value.type>=0);
  assert(value.location.valid() && value.location.file_id()==0 && value.location.source_offset()==4);
  const SemanticTypeInfo type=model->type(value.type);
  assert(type.kind==SEMANTIC_BUILTIN && model->name_text(type.name)=="int");
  const SemanticNameId name=model->find_name("audited_value");
  assert(name!=UINT32_MAX);
  const SemanticLookupResult unqualified=model->lookup_unqualified(0,name);
  assert(unqualified.scope==0 && unqualified.binding_index==0);
  const std::vector<SemanticNameId> path(1,name);
  const SemanticLookupResult qualified=model->lookup_qualified(0,path);
  assert(qualified.scope==0 && qualified.binding_index==0);
  return 0;
}
