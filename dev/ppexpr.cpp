// PA3 controlling-expression tool entry point.
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>

#include "preprocess/expressions/ControlExpression.h"

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
    run_control_expression_tool(source, std::cout);
    return EXIT_SUCCESS;
  }
  catch (const std::exception & error)
  {
    std::cerr << "ERROR: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
