#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::graphics::native_resolve {

// Sampling provenance, not residency. Disjoint tiled atlas rectangles may have
// overlapping memory bounds. No pixels are stored here. Caller holds the global
// critical region when sharing this object with memory-write callbacks.
struct Layout {
  uint32_t base = 0, pitch = 0, format = 0, endian = 0, bytes_per_pixel_log2 = 0;
  bool operator==(const Layout& b) const {
    return base == b.base && pitch == b.pitch && format == b.format &&
           endian == b.endian && bytes_per_pixel_log2 == b.bytes_per_pixel_log2;
  }
};
struct Rect {
  uint32_t left = 0, top = 0, right = 0, bottom = 0;
  bool valid() const { return left < right && top < bottom; }
  bool Overlaps(const Rect& b) const {
    return left < b.right && b.left < right && top < b.bottom && b.top < bottom;
  }
  bool Contains(const Rect& b) const {
    return b.valid() && left <= b.left && top <= b.top &&
           right >= b.right && bottom >= b.bottom;
  }
};
struct Write {
  static constexpr uint32_t kMemoryBytes = 1u << 29;
  Layout layout;
  Rect rect;
  uint32_t extent_start = 0, extent_length = 0;
  // Only exact ordinary tiled 2D writes may set this. Unknown/array producers
  // still invalidate prior provenance through their physical memory bounds.
  bool layout_known = false;
  bool HasValidExtent() const {
    return extent_length && extent_start < kMemoryBytes &&
           uint64_t(extent_start) + extent_length <= kMemoryBytes;
  }
  bool HasKnownRect() const {
    return layout_known && HasValidExtent() && rect.valid() &&
           layout.base < kMemoryBytes && layout.base <= extent_start &&
           !(layout.base & 4095) && layout.pitch && !(layout.pitch & 31) &&
           rect.right <= layout.pitch && rect.bottom <= 8192 &&
           layout.pitch <= 8192 && layout.format < 64 && layout.endian < 8 &&
           layout.bytes_per_pixel_log2 <= 4;
  }
};
class RegionMap {
 public:
  static constexpr size_t kCapacity = 64;
  void Clear() { count_ = 0; }
  size_t size() const { return count_; }
  void Invalidate(uint32_t start, uint32_t length) {
    if (!length) return;
    for (size_t i = 0; i < count_;) {
      if (BytesOverlap(entries_[i], start, length)) Erase(i);
      else ++i;
    }
  }
  void Record(const Write& write, bool authored_native) {
    if (!write.extent_length) return;
    if (!write.HasValidExtent()) {
      // Fail closed on wrapping/invalid input. This discards metadata only,
      // never invalidates texture resources or performs a forced upload.
      Clear();
      return;
    }
    const bool known = write.HasKnownRect();
    for (size_t i = 0; i < count_;) {
      const auto& old = entries_[i];
      if (BytesOverlap(old, write.extent_start, write.extent_length) &&
          (!known || !(old.layout == write.layout) || old.rect.Overlaps(write.rect))) Erase(i);
      else ++i;
    }
    if (!known || !authored_native) return;
    if (count_ == kCapacity) Erase(0);
    entries_[count_++] = write;
  }
  bool Find(const Layout& layout, const Rect& footprint, Rect& out) const {
    for (size_t i = count_; i--;) {
      if (entries_[i].layout == layout && entries_[i].rect.Contains(footprint)) {
        out = entries_[i].rect; return true;
      }
    }
    out = {}; return false;
  }
  // Returning a candidate is not permission to sample outside it: the shader
  // must check the complete bilinear footprint, not only the center coordinate.
  bool Latest(const Layout& layout, Rect& out) const {
    for (size_t i = count_; i--;) {
      if (entries_[i].layout == layout) { out = entries_[i].rect; return true; }
    }
    out = {}; return false;
  }
 private:
  static bool BytesOverlap(const Write& old, uint32_t start, uint32_t length) {
    return uint64_t(old.extent_start) < uint64_t(start) + length &&
           uint64_t(start) < uint64_t(old.extent_start) + old.extent_length;
  }
  void Erase(size_t i) {
    for (; i + 1 < count_; ++i) entries_[i] = entries_[i + 1];
    --count_;
  }
  std::array<Write, kCapacity> entries_{};
  size_t count_ = 0;
};
}  // namespace rex::graphics::native_resolve
