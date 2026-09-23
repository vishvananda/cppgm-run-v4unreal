#pragma once

#include <iosfwd>
#include <string>

// Run PA3's controlling-expression tool over a UTF-8 source buffer. Phase 1-3
// failures are reported by PPTokenizer as exceptions; expression failures are
// isolated to their logical source line.
void run_control_expression_tool(const std::string & source, std::ostream & output);
