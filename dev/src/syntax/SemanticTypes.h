#pragma once
#include "syntax/Parser.h"
#include <iosfwd>

// Build and render the translation-unit-local PA6 semantic graph directly
// from the structured PA5 syntax tree.
void write_semantic_types(const SyntaxTree &tree, std::ostream &output);
