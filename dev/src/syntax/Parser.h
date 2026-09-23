#pragma once
#include "preprocess/tokens/PostTokenPipeline.h"
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

// Owned structured syntax node. The AST is available to later frontend passes;
// textual output is a rendering adapter, never an input to semantic phases.
struct SyntaxNode {
  std::string text;
  std::vector<SyntaxNode> children;
  explicit SyntaxNode(const std::string &label = std::string()) : text(label) {}
  SyntaxNode &add(const SyntaxNode &child) {
    children.push_back(child);
    return *this;
  }
  SyntaxNode &add(SyntaxNode &&child) {
    children.push_back(std::move(child));
    return *this;
  }
};

// Parse one independently preprocessed translation unit from a token span.
SyntaxNode parse_syntax_tree(const std::vector<PostTokenRecord> &tokens,
                             std::size_t first, std::size_t last);
void write_syntax_tree(const SyntaxNode &tree, std::ostream &output);
void parse_and_dump_translation_unit(const std::vector<PostTokenRecord> &tokens,
                                     std::size_t first, std::size_t last,
                                     std::ostream &output);
