#pragma once

#include <cstdint>

namespace rex::graphics::embedded_camera_depth_clear_policy {

struct Draw {
  uint64_t frame = 0, ordinal = 0, vertex_shader = 0, pixel_shader = 0;
  bool selected = false, memexport = false;
  uint32_t surface = 0, depth = 0, control = 0, primitive = 0, vertices = 0;
  uint32_t scale_x = 1, scale_y = 1;
};

// Two observed clear/next-draw pairs in a single manually selected frame.
// A failed copy spends its slot. A different draw cannot be relabeled as the
// immediate alias consumer, and later frames/requests cannot reopen the budget.
struct Budget {
  uint64_t frame = 0, pending_clear = 0, last_clear = 0;
  uint32_t pairs = 0, attempts = 0, missed = 0;
  bool closed = false;

  bool AcceptFrame(const Draw& d) noexcept {
    if (closed) return false;
    if (frame && frame != d.frame) { closed = true; return false; }
    return d.selected && d.frame && d.ordinal && !d.memexport &&
        d.scale_x == 1 && d.scale_y == 1 && d.depth == 0x00010000;
  }

  bool AfterClear(const Draw& d) noexcept {
    if (!AcceptFrame(d) || pairs == 2 || d.ordinal <= last_clear ||
        d.surface != 0x0A020280 || d.control != 0x00008777 ||
        d.vertex_shader != UINT64_C(0x0A6D1DD7767FDF27) ||
        d.pixel_shader != UINT64_C(0x2E372EA28CC404B7) ||
        d.primitive != 8 || d.vertices != 3) return false;
    if (pending_clear) ++missed;
    frame = d.frame;
    pending_clear = last_clear = d.ordinal;
    ++pairs;
    ++attempts;
    return true;
  }

  uint64_t AfterAlias(const Draw& d) noexcept {
    if (!pending_clear) return 0;
    const uint64_t clear = pending_clear;
    pending_clear = 0;
    if (!AcceptFrame(d) || d.ordinal != clear + 1 ||
        d.surface != 0x14010500 || (d.control & 6) != 6 ||
        d.primitive != 4) { ++missed; return 0; }
    ++attempts;
    return clear;
  }
};

struct ReadbackBudget {
  static constexpr uint32_t kRows = 384;
  static constexpr uint64_t kMaximumBytes = UINT64_C(16) * 1024 * 1024;
  uint32_t attempts = 0;
  uint64_t bytes = 0;

  uint64_t Reserve(uint64_t width, uint32_t source_height, uint32_t samples) noexcept {
    if (attempts == 4) return 0;
    ++attempts;
    // This is explicitly the first 384 rows, not a full-resource capture.
    if (!width || width > 1280 || source_height < kRows ||
        (samples != 2 && samples != 4)) return 0;
    const uint64_t size = width * kRows * samples * 8;
    if (size > kMaximumBytes || size > 4 * kMaximumBytes - bytes) return 0;
    bytes += size;
    return size;
  }
};

}  // namespace rex::graphics::embedded_camera_depth_clear_policy
