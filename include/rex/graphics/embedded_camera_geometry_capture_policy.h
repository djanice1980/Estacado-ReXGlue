#pragma once

#include <cstdint>

namespace rex::graphics::embedded_camera_geometry_capture_policy {

inline bool IsPackedPositionShader(uint64_t shader) noexcept {
  return shader == UINT64_C(0x1A0DA8AEF2660AD9) ||
      shader == UINT64_C(0x741FD5FD94E6154D) ||
      shader == UINT64_C(0x2517F03F3BDACF2F) ||
      shader == UINT64_C(0xA6EFBE22D37B8304);
}

// Exact observed geometry families, not an arbitrary memory dump selector.
struct Draw {
  uint64_t frame = 0, ordinal = 0, vertex_shader = 0;
  bool selected_frame = false, memexport = false;
  uint32_t surface = 0, depth = 0, depth_control = 0;
  uint32_t scale_x = 1, scale_y = 1;
};

inline bool IsEligible(const Draw& d) noexcept {
  return d.frame && d.ordinal && d.selected_frame && !d.memexport &&
      d.scale_x == 1 && d.scale_y == 1 && d.depth == 0x00010000 &&
      (d.depth_control & 6) == 6 &&
      ((d.surface == 0x14010500 && IsPackedPositionShader(d.vertex_shader)) ||
       (d.surface == 0x0A020280 &&
        d.vertex_shader == UINT64_C(0x0A6D1DD7767FDF27)));
}

struct DrawBudget {
  static constexpr uint32_t kMaximumDraws = 64;
  uint64_t frame = 0, last_draw = 0;
  uint32_t attempts = 0, dropped = 0;
  bool closed = false;

  bool Select(const Draw& d) noexcept {
    if (closed) return false;
    if (frame && frame != d.frame) { closed = true; return false; }
    if (!IsEligible(d) || d.ordinal <= last_draw) return false;
    frame = d.frame;
    last_draw = d.ordinal;
    if (attempts == kMaximumDraws) { ++dropped; return false; }
    ++attempts;
    return true;
  }
};

// Full ranges or rejection. Unlike the legacy 4-KiB preview, never truncate.
// Reserve before allocation so failures cannot retry or grow without bound.
struct RangeBudget {
  static constexpr uint32_t kMemoryBytes = 512u * 1024u * 1024u;
  static constexpr uint32_t kMaximumRangeBytes = 2u * 1024u * 1024u;
  static constexpr uint32_t kMaximumTotalBytes = 16u * 1024u * 1024u;
  static constexpr uint32_t kMaximumRanges = DrawBudget::kMaximumDraws * 2;
  uint32_t attempts = 0, reserved_bytes = 0;

  uint32_t Reserve(uint32_t address, uint64_t bytes) noexcept {
    if (attempts == kMaximumRanges) return 0;
    ++attempts;
    if (!bytes || bytes > kMaximumRangeBytes || address >= kMemoryBytes ||
        bytes > uint64_t(kMemoryBytes - address) ||
        bytes > uint64_t(kMaximumTotalBytes - reserved_bytes)) return 0;
    reserved_bytes += uint32_t(bytes);
    return uint32_t(bytes);
  }
};

}  // namespace rex::graphics::embedded_camera_geometry_capture_policy
