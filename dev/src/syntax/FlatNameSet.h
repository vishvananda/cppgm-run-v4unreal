#pragma once

#include "preprocess/tokens/PostTokenPipeline.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class FlatNameSet {
  PostTokenBuffer *tokens_;
  std::vector<std::uint32_t> slots_;
  std::size_t size_;

  std::string canonical_name(const std::string &input) const {
    std::string name = input;
    if (name.compare(0, 9, "template ") == 0)
      name.erase(0, 9);
    const std::size_t scope = name.rfind("::");
    if (scope != std::string::npos) name.erase(0, scope + 2);
    const std::size_t angle = name.find('<');
    if (angle != std::string::npos) name.erase(angle);
    if (!name.empty() && name[0] == '~') name.erase(0, 1);
    return name;
  }
  std::uint64_t hash_id(std::uint32_t id) const {
    const std::string &text = tokens_->spelling(id);
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (std::size_t i = 0; i < text.size(); ++i) {
      hash ^= static_cast<unsigned char>(text[i]);
      hash *= UINT64_C(1099511628211);
    }
    return hash;
  }
  std::size_t slot_for(std::uint32_t id) const {
    std::size_t slot = static_cast<std::size_t>(hash_id(id)) &
                       (slots_.size() - 1);
    while (slots_[slot] && slots_[slot] != id + 1)
      slot = (slot + 1) & (slots_.size() - 1);
    return slot;
  }
  void rehash(std::size_t capacity) {
    const std::vector<std::uint32_t> old(slots_);
    slots_.assign(capacity, 0);
    size_ = 0;
    for (std::size_t i = 0; i < old.size(); ++i)
      if (old[i]) insert_id(old[i] - 1);
  }
public:
  explicit FlatNameSet(PostTokenBuffer &tokens) : tokens_(&tokens), size_(0) {}
  bool count(const std::string &name) const {
    std::uint32_t id;
    if (!tokens_->find_spelling(name, &id) || slots_.empty()) return false;
    const std::size_t slot = slot_for(id);
    return slots_[slot] == id + 1;
  }
  void insert(const std::string &input) {
    const std::string name = canonical_name(input);
    std::uint32_t id;
    if (tokens_->find_spelling(name, &id)) insert_id(id);
  }
  void insert_id(std::uint32_t id) {
    if (slots_.empty()) slots_.assign(8, 0);
    const std::size_t slot = slot_for(id);
    if (slots_[slot] == id + 1) return;
    if ((size_ + 1) * 10 >= slots_.size() * 7) {
      rehash(slots_.size() * 2);
      slots_[slot_for(id)] = id + 1;
    } else
      slots_[slot] = id + 1;
    ++size_;
  }
  void erase(const std::string &input) {
    const std::string name = canonical_name(input);
    std::uint32_t id;
    if (tokens_->find_spelling(name, &id)) erase_id(id);
  }
  void erase_id(std::uint32_t id) {
    if (slots_.empty()) return;
    std::size_t slot = slot_for(id);
    if (slots_[slot] != id + 1) return;
    slots_[slot] = 0;
    --size_;
    std::size_t next = (slot + 1) & (slots_.size() - 1);
    while (slots_[next]) {
      const std::uint32_t displaced = slots_[next] - 1;
      slots_[next] = 0;
      --size_;
      insert_id(displaced);
      next = (next + 1) & (slots_.size() - 1);
    }
  }
  const std::vector<std::uint32_t> &slots() const { return slots_; }
};

