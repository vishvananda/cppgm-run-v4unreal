#pragma once
#include "preprocess/tokens/PostTokenPipeline.h"
#include <cstddef>
#include <iosfwd>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

class SyntaxArena {
  struct Block {
    std::unique_ptr<unsigned char[]> storage;
    std::size_t capacity;
    std::size_t used;
    Block(std::size_t size)
        : storage(new unsigned char[size]), capacity(size), used(0) {}
  };
  std::vector<Block> blocks_;
  std::map<std::size_t, std::vector<void *> > free_blocks_;
public:
  SyntaxArena() {}
  SyntaxArena(const SyntaxArena &) = delete;
  SyntaxArena &operator=(const SyntaxArena &) = delete;
  void *allocate(std::size_t bytes, std::size_t alignment);
  void deallocate(void *ptr, std::size_t bytes, std::size_t alignment);
};

template <class T> class SyntaxArenaAllocator {
  template <class U> friend class SyntaxArenaAllocator;
  SyntaxArena *arena_;
public:
  typedef T value_type;
  typedef T *pointer;
  typedef const T *const_pointer;
  typedef T &reference;
  typedef const T &const_reference;
  typedef std::size_t size_type;
  typedef std::ptrdiff_t difference_type;
  typedef std::true_type propagate_on_container_move_assignment;
  typedef std::true_type propagate_on_container_swap;
  template <class U> struct rebind { typedef SyntaxArenaAllocator<U> other; };

  explicit SyntaxArenaAllocator(SyntaxArena *arena = 0) : arena_(arena) {}
  template <class U>
  SyntaxArenaAllocator(const SyntaxArenaAllocator<U> &other)
      : arena_(other.arena_) {}
  pointer allocate(size_type count) {
    if (count > static_cast<size_type>(-1) / sizeof(T))
      throw std::bad_alloc();
    const size_type bytes = count * sizeof(T);
    return static_cast<pointer>(arena_ ? arena_->allocate(bytes, alignof(T))
                                       : ::operator new(bytes));
  }
  void deallocate(pointer ptr, size_type count) noexcept {
    if (!arena_) ::operator delete(ptr);
    else arena_->deallocate(ptr, count * sizeof(T), alignof(T));
  }
  SyntaxArena *arena() const { return arena_; }
  template <class U> bool operator==(const SyntaxArenaAllocator<U> &other) const {
    return arena_ == other.arena();
  }
  template <class U> bool operator!=(const SyntaxArenaAllocator<U> &other) const {
    return !(*this == other);
  }
};

struct SyntaxLocation {
  // Pack a 20-bit file identity and 44-bit byte offset into one word. Line and
  // column are derived from the retained immutable source buffer when needed.
  std::uint64_t packed;
  SyntaxLocation() : packed(UINT64_MAX) {}
  bool valid() const { return packed != UINT64_MAX; }
  std::uint32_t file_id() const {
    return static_cast<std::uint32_t>(packed >> 44);
  }
  std::size_t source_offset() const {
    return static_cast<std::size_t>(packed & ((UINT64_C(1) << 44) - 1));
  }
  void set(std::uint32_t file, std::size_t offset) {
    if (file >= (UINT32_C(1) << 20) ||
        static_cast<std::uint64_t>(offset) >= (UINT64_C(1) << 44))
      throw std::runtime_error("source location identity is out of range");
    packed = (static_cast<std::uint64_t>(file) << 44) |
             static_cast<std::uint64_t>(offset);
  }
};

// Owned structured syntax node. Locations point into the immutable source-file
// buffers owned by SyntaxTree; textual output is a view, never a phase input.
struct SyntaxNode {
  std::string text;
  std::vector<SyntaxNode, SyntaxArenaAllocator<SyntaxNode> > children;
  SyntaxLocation location;
  explicit SyntaxNode(const std::string &label = std::string(),
                      SyntaxArena *arena = 0)
      : text(label), children(SyntaxArenaAllocator<SyntaxNode>(arena)) {}
  void include_location(const SyntaxNode &child) {
    if (!child.location.valid()) return;
    if (!location.valid() ||
        (location.file_id() == child.location.file_id() &&
         child.location.source_offset() < location.source_offset()))
      location = child.location;
  }
  SyntaxNode &add(const SyntaxNode &child) {
    include_location(child);
    children.push_back(child);
    return *this;
  }
  SyntaxNode &add(SyntaxNode &&child) {
    include_location(child);
    children.push_back(std::move(child));
    return *this;
  }
};

struct SyntaxTree {
  // Declared first so all arena-backed node vectors are destroyed before the
  // slab. The pointer target remains stable when the tree is moved.
  std::unique_ptr<SyntaxArena> arena;
  std::vector<PostTokenSourceFile> source_files;
  SyntaxNode root;
};

// Parse one independently preprocessed translation unit from a compact token
// span. The resulting tree takes ownership of the immutable source buffers.
SyntaxTree parse_syntax_tree(PostTokenBuffer &tokens,
                             std::size_t first, std::size_t last);
void write_syntax_tree(const SyntaxTree &tree, std::ostream &output);
void parse_and_dump_translation_unit(PostTokenBuffer &tokens,
                                     std::size_t first, std::size_t last,
                                     std::ostream &output);
