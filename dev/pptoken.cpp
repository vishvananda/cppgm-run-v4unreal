#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

#include "preprocess/tokens/DebugPPTokenStream.h"
#include "preprocess/tokens/PPTokenizer.h"

int main(int argc, char ** argv)
{
  (void)argc;
  (void)argv;
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(NULL);
  try
  {
    const std::string source((std::istreambuf_iterator<char>(std::cin)),
                             std::istreambuf_iterator<char>());
    DebugPPTokenStream output;
    PPTokenizer tokenizer(source, output);
    tokenizer.tokenize();
    return EXIT_SUCCESS;
  }
  catch (const std::exception & error)
  {
    std::cerr << "ERROR: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
