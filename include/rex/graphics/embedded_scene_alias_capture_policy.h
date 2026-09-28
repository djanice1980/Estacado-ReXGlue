#pragma once

#include <cstdint>

namespace rex::graphics::embedded_scene_alias_capture_policy {

// One manually selected frame, eight complete excursions through the observed
// native scene alias. Reserve both copies before starting a pair. Failed
// allocations consume their reservation too, preventing diagnostic retry loops.
struct Budget {
  static constexpr uint32_t kMaximumPairs = 8;
  static constexpr uint64_t kMaximumBytes = UINT64_C(128) * 1024 * 1024;
  static constexpr uint32_t kColorKey = 0x00C88300, kDepthKey = 0x00708000;
  uint64_t frame = 0, reserved_bytes = 0;
  uint32_t pairs = 0, attempts = 0, queued = 0, failures = 0, dropped = 0;

  static uint64_t CopyBytes(uint32_t start, uint32_t end) noexcept {
    // Full tile rows only, without EDRAM wrapping, at native 1280-wide 2x MSAA.
    if (start < 768 || start >= end || end > 1536 || start % 16 || end % 16)
      return 0;
    return UINT64_C(1280) * ((end - start) / 16 * 8) * 2 * 8;
  }
  uint32_t Reserve(uint64_t frame_id, uint32_t start, uint32_t end) noexcept {
    const uint64_t bytes = CopyBytes(start, end) * 2;
    if (!frame_id || (frame && frame != frame_id) || !bytes ||
        pairs == kMaximumPairs || bytes > kMaximumBytes - reserved_bytes) {
      ++dropped;
      return 0;
    }
    frame = frame_id;
    reserved_bytes += bytes;
    return ++pairs;
  }
};

}  // namespace rex::graphics::embedded_scene_alias_capture_policy
