#pragma once

#include <array>
#include <cstdint>

namespace rex::graphics::pc_scene_color_history {

// Reviewed Xenos format26 uses an unsigned normalized view of typeless RGBA16
// storage. Sampling applies the separately supplied signed exponent adjustment.
constexpr bool SupportedSource(uint32_t guest_format, uint32_t resource_format,
                               uint32_t signs, bool integer_sampling,
                               uint32_t width, uint32_t height) noexcept {
  return guest_format == 26 && (resource_format == 9 || resource_format == 11) &&
         signs == 0 && !integer_sampling && width == 1280 && height == 720;
}

// Owns rendered color frame identity only. Adjacency is NOT temporal validity:
// cuts, camera/object motion, exposure and deformation have separate contracts.
// Single command-processor thread and its ordered graphics queue only.
struct State {
  static constexpr int kSlots = 4;
  struct Slot { uint64_t frame = 0, last_submission = 0; };
  std::array<Slot, kSlots> slots{};
  int current = -1, previous = -1, pending = -1;
  uint64_t pending_frame = 0, last_finished = 0, epoch = 1;
  bool failed = false, copied = false;

  // Invalidating semantic ownership must NEVER discard resource fence values.
  void Invalidate() noexcept {
    current = previous = pending = -1;
    pending_frame = 0;
    failed = false;
    copied = false;
    ++epoch;
  }
  int Reserve(uint64_t frame, uint64_t completed) noexcept {
    if (!frame || frame <= last_finished) return -1;
    if (pending_frame && pending_frame != frame) Invalidate();
    if (pending_frame == frame) { failed = true; return -1; }
    pending_frame = frame;
    copied = false;
    for (int i = 0; i < kSlots; ++i) {
      if (i != current && i != previous && slots[i].last_submission <= completed) {
        pending = i;
        return i;
      }
    }
    failed = true;
    return -1;
  }
  void Copied(uint64_t submission) noexcept {
    if (pending >= 0 && submission) {
      slots[pending].last_submission = submission;
      copied = true;
    }
    else failed = true;
  }
  void Fail(uint64_t frame) noexcept {
    if (frame > last_finished) { pending_frame = frame; failed = true; }
  }
  bool Finish(uint64_t frame) noexcept {
    if (!frame || frame <= last_finished) { Invalidate(); return false; }
    last_finished = frame;
    if (failed || !copied || pending_frame != frame || pending < 0) {
      Invalidate();
      return false;
    }
    previous = current >= 0 && slots[current].frame == frame - 1 ? current : -1;
    current = pending;
    slots[current].frame = frame;
    pending = -1;
    pending_frame = 0;
    copied = false;
    return true;
  }
  // A consumer borrows only on this queue; this extends both reuse fences.
  bool Use(uint64_t submission) noexcept {
    if (current < 0 || !submission) return false;
    for (int i : {current, previous}) if (i >= 0) {
      if (slots[i].last_submission < submission) slots[i].last_submission = submission;
    }
    return true;
  }
};

}  // namespace rex::graphics::pc_scene_color_history
