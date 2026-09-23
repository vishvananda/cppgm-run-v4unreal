#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "preprocess/Preprocessor.h"
#include "preprocess/tokens/PostTokenPipeline.h"

namespace {
std::string quote(const std::string & s)
{
  std::string result("\"");
  for (std::size_t i = 0; i < s.size(); ++i)
  {
    if (s[i] == '\\' || s[i] == '"') result.push_back('\\');
    result.push_back(s[i]);
  }
  result.push_back('"');
  return result;
}
}

int main(int argc, char ** argv)
{
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(NULL);
  try
  {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    if (args.size() < 3 || args[0] != "-o")
      throw std::logic_error("usage: preproc -o <outfile> <source...>");
    const std::string outfile = args[1];
    std::vector<std::string> sources(args.begin() + 2, args.end());
    std::ofstream out(outfile.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open output file");
    out << "preproc " << sources.size() << '\n';

    // The course contract obtains one asctime timestamp for the complete run.
    const std::time_t now = std::time(0);
    std::tm * local = std::localtime(&now);
    const char * stamp = local ? std::asctime(local) : 0;
    std::string date = "\"Jan  1 1970\"", clock = "\"00:00:00\"";
    if (stamp && std::string(stamp).size() >= 24)
    {
      date = quote(std::string(stamp + 4, 7) + std::string(stamp + 20, 4));
      clock = quote(std::string(stamp + 11, 8));
    }
    bool invalid = false;
    std::unique_ptr<IPPTokenStream> output = create_posttoken_stream(out, &invalid);
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
      out << "sof " << sources[i] << '\n';
      const std::vector<std::string> one(1, sources[i]);
      run_preprocessor_files(one, *output, date, clock);
    }
    out.flush();
    if (!out) throw std::runtime_error("failed writing output");
    return invalid ? EXIT_FAILURE : EXIT_SUCCESS;
  }
  catch (const std::exception & error)
  {
    std::cerr << "ERROR: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
