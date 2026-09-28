#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace rex::input::mnk::test_tap_policy {
// Opt-in developer retention of an actual window key-down. No action source,
// polling thread or input injection is created here. Time never advances guest
// state, and a missed/abandoned tap expires even if no guest poll occurs.
class BoundedTaps {
 public:
  void KeyDown(uint16_t key, uint32_t hold_ms, uint64_t now_ms) {
    if (key >= expiry_.size() || key == 0x1B || !hold_ms || hold_ms > 250) return;
    if (now_ms > (std::numeric_limits<uint64_t>::max)() - hold_ms) return;
    expiry_[key] = now_ms + hold_ms;
  }
  bool Active(uint16_t key, uint64_t now_ms) const {
    return key < expiry_.size() && expiry_[key] > now_ms;
  }
  void Reset() { expiry_.fill(0); }
 private:
  std::array<uint64_t, 256> expiry_{};
};

// A deliberately enabled offline test may map real I/J/K/L key events to
// ordinary controller look axes. Opposing keys cancel; neutral/disabled input
// preserves the mouse result exactly. No guest camera state is written here.
inline int16_t CameraAxis(bool enabled, int16_t mouse_axis,
                          bool negative, bool positive) {
  if (!enabled || negative == positive) return mouse_axis;
  return positive ? INT16_MAX : -INT16_MAX;
}
}
