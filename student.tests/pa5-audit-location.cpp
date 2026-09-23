#include "preprocess/Preprocessor.h"
#include "syntax/Parser.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string path(argv[1]);
  std::ifstream input(path.c_str(), std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  if (!input && !input.eof()) return 3;

  PostTokenBuffer tokens;
  bool invalid = false;
  std::unique_ptr<IPPTokenStream> collector =
      create_posttoken_collector(tokens, &invalid);
  std::vector<std::string> sources(1, path);
  run_preprocessor_files(sources, *collector, "\"Jan  1 1970\"",
                         "\"00:00:00\"");
  if (invalid || tokens.size() < 2) return 4;
  const std::size_t invocation = source.find("DECL =");
  if (invocation == std::string::npos) return 5;
  const PostTokenRecord &first = tokens[0];
  if (first.source_offset != invocation || first.line != 2 || first.column != 1 ||
      first.file_id != 0)
    return 6;

  std::size_t end = 0;
  while (end < tokens.size() && tokens[end].kind != PostTokenEof) ++end;
  SyntaxTree tree = parse_syntax_tree(tokens, 0, end);
  if (tree.source_files.empty() || !tree.source_files[0].contents ||
      *tree.source_files[0].contents != source)
    return 7;
  if (!tree.root.location.valid() || tree.root.location.file_id() != first.file_id ||
      tree.root.location.source_offset() != invocation)
    return 8;
  return 0;
}
