#include "syntax/Parser.h"

void *SyntaxArena::allocate(std::size_t bytes, std::size_t alignment) {
  if (!bytes) bytes = 1;
  if (!alignment || (alignment & (alignment - 1)))
    throw std::bad_alloc();
  std::map<std::size_t, std::vector<void *> >::iterator reusable =
      free_blocks_.find(bytes);
  if (reusable != free_blocks_.end() && !reusable->second.empty()) {
    void *ptr = reusable->second.back();
    reusable->second.pop_back();
    return ptr;
  }
  for (std::size_t i = 0; i < blocks_.size(); ++i) {
    Block &block = blocks_[i];
    const std::size_t offset =
        (block.used + alignment - 1) & ~(alignment - 1);
    if (offset <= block.capacity && bytes <= block.capacity - offset) {
      block.used = offset + bytes;
      return block.storage.get() + offset;
    }
  }
  if (bytes > static_cast<std::size_t>(-1) - (alignment - 1))
    throw std::bad_alloc();
  const std::size_t minimum = bytes + alignment - 1;
  const std::size_t capacity = minimum > 65536 ? minimum : 65536;
  blocks_.push_back(Block(capacity));
  Block &block = blocks_.back();
  const std::size_t offset = (block.used + alignment - 1) & ~(alignment - 1);
  block.used = offset + bytes;
  return block.storage.get() + offset;
}

void SyntaxArena::deallocate(void *ptr, std::size_t bytes,
                             std::size_t alignment) {
  (void)alignment;
  if (!ptr) return;
  try {
    free_blocks_[bytes].push_back(ptr);
  } catch (...) {
    // The slab still owns the storage. Reuse is an optimization; failure to
    // grow its free list must not make standard-container destruction throw.
  }
}

