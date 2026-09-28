#pragma once

#include <cstdint>

namespace rex::graphics::embedded_manual_capture_policy {

enum class Action {
  kNone,
  kArm,
  kCapture,
};

struct State {
  uint64_t armed_generation = 0;
  uint64_t consumed_generation = 0;
};

inline bool IsPending(uint64_t requested_generation, const State& state) {
  return requested_generation != state.consumed_generation;
}

// True only while the frame after the arm-only swap is being collected. The
// request may arrive after draws have already been submitted in the preceding
// frame, so IsPending alone must never be used as the detailed-frame gate.
inline bool IsCompleteFrameArmed(const State& state) {
  return state.armed_generation != 0;
}

// The first completed swap after a physical request only arms capture. This
// discards a potentially partial frame if the key edge arrived after some
// draws. The next completed swap therefore contains one whole frame of state.
inline Action Advance(uint64_t requested_generation, State& state,
                      uint64_t* capture_generation = nullptr) {
  if (capture_generation) {
    *capture_generation = 0;
  }
  if (state.armed_generation) {
    const uint64_t completed_generation = state.armed_generation;
    state.armed_generation = 0;
    state.consumed_generation = completed_generation;
    if (capture_generation) {
      *capture_generation = completed_generation;
    }
    return Action::kCapture;
  }
  if (requested_generation != state.consumed_generation) {
    state.armed_generation = requested_generation;
    return Action::kArm;
  }
  return Action::kNone;
}

}  // namespace rex::graphics::embedded_manual_capture_policy
