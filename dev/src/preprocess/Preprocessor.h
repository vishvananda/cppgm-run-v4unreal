#pragma once

#include <string>
#include <vector>

#include "preprocess/tokens/IPPTokenStream.h"

// Run each primary source as an independent translation unit and stream its
// post-macro preprocessing tokens to a typed phase-5/7 consumer. Includes are
// processed recursively through the same consumer and translation-unit state.
// `date_literal` and `time_literal` are already quoted ordinary string tokens.
void run_preprocessor_files(const std::vector<std::string> & sources,
                            IPPTokenStream & output,
                            const std::string & date_literal,
                            const std::string & time_literal);
