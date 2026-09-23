#include <cstdlib>
#include <iostream>

#include "preprocess/tokens/PostTokenPipeline.h"

int main(int argc, char ** argv)
{
  (void)argc;
  (void)argv;
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(NULL);
  try
  {
    run_posttoken_tool(std::cin, std::cout);
    return EXIT_SUCCESS;
  }
  catch (const std::exception & error)
  {
    std::cerr << "ERROR: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
