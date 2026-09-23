#include "syntax/Parser.h"
#include <ostream>

namespace {
void render(const SyntaxNode &x, std::ostream &out, unsigned depth) {
  for (unsigned i = 0; i < depth; ++i)
    out << "  ";
  out << x.text << '\n';
  for (std::size_t i = 0; i < x.children.size(); ++i)
    render(x.children[i], out, depth + 1);
}

} // namespace
void write_syntax_tree(const SyntaxTree &tree, std::ostream &output) {
  render(tree.root, output, 0);
}

void parse_and_dump_translation_unit(PostTokenBuffer &tokens,
                                    std::size_t first, std::size_t last,
                                    std::ostream &output) {
  write_syntax_tree(parse_syntax_tree(tokens, first, last), output);
}
