#pragma once

#include "syntax/Parser.h"

struct Tok {
  const std::string &s;
  PostTokenKind kind;
  SyntaxLocation location;
  Tok(const std::string &a, PostTokenKind k) : s(a), kind(k) {}
  Tok(const PostTokenBuffer &buffer, const PostTokenRecord &record)
      : s(buffer.spelling(record.spelling_id)),
        kind(static_cast<PostTokenKind>(record.kind)) {
    location.set(record.file_id, record.source_offset);
  }
};

class TokenView {
  const PostTokenBuffer &records_;
  std::size_t begin_, end_;
public:
  TokenView(const PostTokenBuffer &records, std::size_t begin, std::size_t end)
      : records_(records), begin_(begin), end_(end) {}
  std::size_t size() const { return end_ - begin_; }
  Tok operator[](std::size_t index) const {
    return Tok(records_, records_[begin_ + index]);
  }
};
