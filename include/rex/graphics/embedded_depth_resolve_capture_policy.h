#pragma once

#include <cstdint>

#include <rex/graphics/embedded_geometry_readback_policy.h>

namespace rex::graphics::embedded_depth_resolve_capture_policy {

// Observed native 720p D24FS8 resolve signature. A selected frame may contain
// several tiles or views; this observer preserves each range separately and
// does not identify a complete main-camera surface from addresses alone.
struct Resolve {
  uint64_t frame = 0;
  uint64_t ordinal = 0;
  bool selected_frame = false;
  bool succeeded = false;
  bool scaled = false;
  uint32_t scale_x = 1, scale_y = 1;
  uint32_t control = 0;
  uint32_t surface = 0;
  uint32_t depth = 0;
  uint32_t destination_info = 0;
  uint32_t destination_pitch = 0;
  uint32_t destination_base = 0;
  uint32_t written_address = 0;
  uint32_t written_bytes = 0;
};

inline bool IsEligible(const Resolve& r) {
  return r.frame && r.ordinal && r.selected_frame && r.succeeded &&
      !r.scaled && r.scale_x == 1 && r.scale_y == 1 &&
      r.control == 0x00000004 && r.surface == 0x14010500 &&
      r.depth == 0x00010000 && r.destination_info == 0x00004302 &&
      r.destination_pitch == 0x02D00500 && r.written_address &&
      r.written_address == r.destination_base &&
      !(r.written_address & 3) && !(r.written_bytes & 3) &&
      embedded_geometry_readback_policy::TextureBytes(
          r.written_address, r.written_bytes, 0) == r.written_bytes &&
      r.written_bytes;
}

class State {
 public:
  static constexpr uint32_t kMaximumCopies = 4;

  // Reserve before allocation: failure must not retry indefinitely. Only one
  // frame is captured, even if another screenshot request arrives later.
  uint32_t Select(const Resolve& r) {
    if (finished_) return 0;
    if (frame_ && frame_ != r.frame) {
      finished_ = true;
      return 0;
    }
    if (!IsEligible(r) || r.ordinal <= last_ordinal_) return 0;
    frame_ = r.frame;
    last_ordinal_ = r.ordinal;
    if (++copies_ == kMaximumCopies) finished_ = true;
    return copies_;
  }

 private:
  uint64_t frame_ = 0, last_ordinal_ = 0;
  uint32_t copies_ = 0;
  bool finished_ = false;
};

}  // namespace rex::graphics::embedded_depth_resolve_capture_policy
