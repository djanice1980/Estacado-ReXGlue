#pragma once

#include <cstdint>

namespace rex::graphics::embedded_camera_history_capture_policy {

// Metadata only, armed at an actual completed swap rather than at a mid-frame
// key edge. Existing single-frame capture policies remain independent.
struct Budget {
  static constexpr uint64_t kMotionPixelShader = UINT64_C(0x7EF6E3B55D32AEB4);
  static constexpr uint32_t kFrames = 2, kMaximumFrames = 6, kMaximumDraws = 128;
  uint64_t generation = 0, first_frame = 0;
  uint64_t observed[kMaximumFrames]{};
  uint32_t captured[kMaximumFrames]{}, dropped[kMaximumFrames]{}, failures[kMaximumFrames]{};
  uint32_t frame_count = kFrames;
  uint32_t finished = 0;
  bool aborted = false, closed = false;

  bool Arm(uint64_t request, uint64_t after_swap, uint32_t frames = kFrames) noexcept {
    // Preserve the two-frame diagnostic. The six-frame opt-in covers the
    // observed producer/consumer arena delay without an unbounded recorder.
    if (generation || closed || !request || (frames != kFrames && frames != kMaximumFrames) ||
        after_swap > UINT64_MAX - frames) return false;
    frame_count = frames;
    generation = request;
    first_frame = after_swap + 1;
    return true;
  }
  bool Active(uint64_t frame) const noexcept {
    return generation && !closed && !aborted && finished < frame_count && frame == first_frame + finished;
  }
  // Two owned-copy proof frames begin at index2 in the six-frame window,
  // covering the observed CPU publication delay. This is selection, not a
  // presumption that CPU/GPU records belong together; evidence must still join.
  bool BeginOwnedProof(uint64_t frame) const noexcept {
    return frame_count == kMaximumFrames && Active(frame) && finished == 2;
  }
  bool Select(uint64_t frame, uint64_t draw, uint64_t pixel_shader) noexcept {
    if (!generation || closed || aborted || frame < first_frame) return false;
    if (!Active(frame) || draw != observed[finished] + 1) { aborted = true; return false; }
    ++observed[finished];
    if (pixel_shader != kMotionPixelShader) return false;
    if (captured[finished] == kMaximumDraws) { ++dropped[finished]; return false; }
    ++captured[finished];
    return true;
  }
  void Failure(uint64_t frame) noexcept {
    if (Active(frame)) ++failures[finished];
  }
  // -1: inactive; -2: an armed frame boundary was lost. A normal index can
  // still have failed/dropped metadata and must not imply acceptance.
  int Finish(uint64_t frame, uint64_t submitted_draws) noexcept {
    if (!generation || closed || frame < first_frame) return -1;
    if (finished >= frame_count || frame != first_frame + finished) {
      aborted = closed = true;
      return -2;
    }
    const uint32_t index = finished++;
    if (observed[index] != submitted_draws) aborted = true;
    if (finished == frame_count || aborted) closed = true;
    return int(index);
  }
};

}  // namespace rex::graphics::embedded_camera_history_capture_policy
