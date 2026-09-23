#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "preprocess/tokens/IPPTokenStream.h"

enum PostTokenKind {
  PostTokenIdentifier,
  PostTokenSimple,
  PostTokenLiteral,
  PostTokenUserDefinedLiteral,
  PostTokenInvalid,
  PostTokenEof
};

// A translation-unit token is a compact ID tuple. Source spellings and source
// file names are interned once in PostTokenBuffer; repeated identifiers and
// punctuation do not own a string per token. The tape is retained only because
// the syntax parser has bounded speculative lookahead and cursor rollback.
struct PostTokenRecord {
  std::size_t source_offset;
  std::uint32_t spelling_id;
  std::uint32_t file_id;
  std::uint32_t line;
  std::uint32_t column;
  std::uint8_t kind;
  PostTokenRecord(PostTokenKind k = PostTokenEof,
                  std::uint32_t spelling = 0,
                  std::uint32_t file = 0,
                  std::size_t ln = 0,
                  std::size_t col = 0,
                  std::size_t offset = 0)
      : source_offset(offset), spelling_id(spelling), file_id(file),
        line(static_cast<std::uint32_t>(ln)),
        column(static_cast<std::uint32_t>(col)),
        kind(static_cast<std::uint8_t>(k)) {}
};

struct PostTokenSourceFile {
  std::string path;
  std::shared_ptr<const std::string> contents;
};

class PostTokenBuffer {
  struct InternSlot {
    std::uint64_t hash;
    std::uint32_t id;
    bool occupied;
    InternSlot() : hash(0), id(0), occupied(false) {}
  };
  std::vector<std::string> spellings_;
  std::vector<InternSlot> spelling_index_;
  std::vector<std::string> files_;
  std::vector<InternSlot> file_index_;
  std::vector<std::shared_ptr<const std::string> > source_buffers_;
  std::vector<PostTokenRecord> records_;
  std::string cached_file_name_;
  std::uint32_t cached_file_id_;
  bool has_cached_file_;

  std::uint32_t intern_file(const std::string &file);
  std::uint32_t intern(std::vector<std::string> &values,
                       std::vector<InternSlot> &index,
                       const std::string &text);
public:
  PostTokenBuffer() : cached_file_id_(0), has_cached_file_(false) {}
  PostTokenBuffer(const PostTokenBuffer &) = delete;
  PostTokenBuffer &operator=(const PostTokenBuffer &) = delete;

  std::size_t size() const { return records_.size(); }
  const PostTokenRecord &operator[](std::size_t i) const { return records_[i]; }
  const std::string &spelling(std::uint32_t id) const { return spellings_[id]; }
  const std::string &file(std::uint32_t id) const { return files_[id]; }
  bool find_spelling(const std::string &text, std::uint32_t *id) const;
  void retain_source_buffer(const std::string &file,
                            const std::shared_ptr<const std::string> &source);
  std::vector<PostTokenSourceFile> take_source_files();
  void append(PostTokenKind kind, const std::string &spelling,
              const std::string &file, std::size_t line, std::size_t column,
              std::size_t source_offset);
};

// Typed PA2 phase-5/7 consumer shared by the posttoken tool and production
// preprocessing. Text dumps are output adapters; the production parser reads
// a single compact token tape, not a textual dump or a second owning token
// vector.
std::unique_ptr<IPPTokenStream> create_posttoken_stream(std::ostream &output,
                                                        bool *saw_invalid = 0);
void run_posttoken_tool(std::istream &input, std::ostream &output);
std::unique_ptr<IPPTokenStream> create_posttoken_collector(
    PostTokenBuffer &records, bool *saw_invalid = 0);
