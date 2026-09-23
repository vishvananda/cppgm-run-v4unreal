#pragma once

#include <iosfwd>
#include <memory>

#include "preprocess/tokens/IPPTokenStream.h"

// Typed PA2 phase-5/7 consumer shared by the posttoken tool and production
// preprocessing. Tokens are delivered directly through IPPTokenStream; the
// textual token dump is only its output adapter.
std::unique_ptr<IPPTokenStream> create_posttoken_stream(std::ostream & output,
                                                        bool * saw_invalid = 0);
void run_posttoken_tool(std::istream & input, std::ostream & output);
