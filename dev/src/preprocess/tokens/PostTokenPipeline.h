#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "preprocess/tokens/IPPTokenStream.h"

// Compact typed lexical event used by production frontend consumers. `kind` is
// one of identifier, simple, literal, user-defined-literal, invalid, or eof;
// spellings remain source token spellings (literal decoding is validated by
// PA2 but does not replace the source-faithful syntax).
struct PostTokenRecord
{
  std::string kind;
  std::string spelling;
  PostTokenRecord(const std::string & k = std::string(),
                  const std::string & s = std::string())
      : kind(k), spelling(s) {}
};

// Typed PA2 phase-5/7 consumer shared by the posttoken tool and production
// preprocessing. Tokens are delivered directly through IPPTokenStream; the
// textual token dump is only its output adapter.
std::unique_ptr<IPPTokenStream> create_posttoken_stream(std::ostream & output,
                                                        bool * saw_invalid = 0);
void run_posttoken_tool(std::istream & input, std::ostream & output);
std::unique_ptr<IPPTokenStream> create_posttoken_collector(
    std::vector<PostTokenRecord> & records, bool * saw_invalid = 0);
