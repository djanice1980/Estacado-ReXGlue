/**
 * @file input/mnk/mouse_wheel_policy.h
 * @brief Bounded physical mouse-wheel pulse queue for controller bindings.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::input::mnk::mouse_wheel_policy {

enum class Direction : int8_t { kNone = 0, kUp = 1, kDown = -1 };

class PulseQueue {
 public:
  static constexpr size_t kCapacity = 32;

  void PushScrollY(int32_t scroll_y) noexcept {
    if (!scroll_y) {
      return;
    }
    const Direction direction =
        scroll_y > 0 ? Direction::kUp : Direction::kDown;
    const uint32_t magnitude = scroll_y == INT32_MIN
                                   ? uint32_t(INT32_MAX) + 1u
                                   : uint32_t(scroll_y < 0 ? -scroll_y
                                                           : scroll_y);
    const size_t pulses = std::max<size_t>(
        1, std::min<size_t>(kCapacity, magnitude / kScrollPerDetent));
    for (size_t pulse = 0; pulse < pulses && size_ < kCapacity; ++pulse) {
      entries_[(head_ + size_) % kCapacity] = direction;
      ++size_;
    }
  }

  Direction Pop() noexcept {
    if (!size_) {
      return Direction::kNone;
    }
    const Direction direction = entries_[head_];
    head_ = (head_ + 1) % kCapacity;
    --size_;
    return direction;
  }

  void Clear() noexcept {
    head_ = 0;
    size_ = 0;
  }

  size_t size() const noexcept { return size_; }

 private:
  static constexpr uint32_t kScrollPerDetent = 120;
  std::array<Direction, kCapacity> entries_{};
  size_t head_ = 0;
  size_t size_ = 0;
};

}  // namespace rex::input::mnk::mouse_wheel_policy
