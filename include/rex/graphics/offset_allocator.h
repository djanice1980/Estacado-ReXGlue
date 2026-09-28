#pragma once

#include <cstdint>
#include <iterator>
#include <map>

namespace rex::graphics {

// First-fit allocator of aligned ranges within [0, capacity), coalescing freed
// neighbors (V300). The bookkeeping lives on the host; the managed memory (a
// GPU heap) is never touched. Used by the D3D12 texture heap pool, where
// allocations happen only when textures are created or destroyed.
class OffsetAllocator {
 public:
  explicit OffsetAllocator(uint64_t capacity) : capacity_(capacity), free_bytes_(capacity) {
    if (capacity) free_.emplace(0, capacity);
  }

  // alignment must be a power of two.
  bool Allocate(uint64_t size, uint64_t alignment, uint64_t& offset_out) {
    if (!size || !alignment || (alignment & (alignment - 1))) return false;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
      const uint64_t begin = it->first;
      const uint64_t end = it->first + it->second;
      const uint64_t aligned = (begin + alignment - 1) & ~(alignment - 1);
      if (aligned < begin || aligned >= end || end - aligned < size) continue;
      free_.erase(it);
      if (aligned > begin) free_.emplace(begin, aligned - begin);
      if (aligned + size < end) free_.emplace(aligned + size, end - (aligned + size));
      free_bytes_ -= size;
      offset_out = aligned;
      return true;
    }
    return false;
  }

  void Free(uint64_t offset, uint64_t size) {
    uint64_t begin = offset;
    uint64_t end = offset + size;
    auto next = free_.lower_bound(offset);
    if (next != free_.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == begin) {
        begin = previous->first;
        free_.erase(previous);
      }
    }
    if (next != free_.end() && next->first == end) {
      end = next->first + next->second;
      free_.erase(next);
    }
    free_.emplace(begin, end - begin);
    free_bytes_ += size;
  }

  uint64_t capacity() const { return capacity_; }
  uint64_t free_bytes() const { return free_bytes_; }
  size_t free_ranges() const { return free_.size(); }

 private:
  uint64_t capacity_;
  uint64_t free_bytes_;
  std::map<uint64_t, uint64_t> free_;  // offset -> size
};

}  // namespace rex::graphics
