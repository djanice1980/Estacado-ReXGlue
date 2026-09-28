#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace rex::graphics::embedded_camera_draw_capture_policy {

// Owned bytes from the actual dense vertex-float upload, plus the shader's
// sparse register map. No reconstructed matrix or assumed register layout.
struct Constants {
  bool valid = false;
  uint32_t count = 0;
  std::array<uint64_t, 4> map{};
  std::array<uint32_t, 256 * 4> words{};

  void Capture(const void* upload, uint32_t bytes, uint32_t float_count,
               const uint64_t* bitmap) noexcept {
    *this = {};
    if (!bitmap || float_count > 256) return;
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = bitmap[i];
      while (bits) { bits &= bits - 1; ++mapped; }
    }
    if (mapped != float_count || bytes < float_count * 16 ||
        (float_count && !upload)) return;
    if (float_count) std::memcpy(words.data(), upload, float_count * 16);
    std::memcpy(map.data(), bitmap, sizeof(map));
    count = float_count;
    valid = true;
  }
};

struct Budget {
  static constexpr uint32_t kMaximumDraws = 2048;
  uint64_t frame = 0;
  uint64_t last_draw = 0;
  uint32_t count = 0;
  uint64_t dropped = 0;
  bool closed = false;

  bool Select(bool selected, uint64_t next_frame, uint64_t draw) noexcept {
    if (closed) return false;
    if (frame && frame != next_frame) { closed = true; return false; }
    if (!selected || !next_frame || !draw || draw <= last_draw) return false;
    if (!frame) frame = next_frame;
    last_draw = draw;
    if (count == kMaximumDraws) { ++dropped; return false; }
    ++count;
    return true;
  }
};

}  // namespace rex::graphics::embedded_camera_draw_capture_policy
