#pragma once

#include <string>

#include "preprocess/tokens/IPPTokenStream.h"

// PA1's phase-1/2 source cursor and phase-3 tokenizer.  The source is kept as
// one immutable UTF-8 buffer; tokens are delivered as they are recognized.
class PPTokenizer
{
public:
  PPTokenizer(const std::string & source, IPPTokenStream & output);
  void tokenize();

private:
  const std::string & source_;
  IPPTokenStream & output_;
};
