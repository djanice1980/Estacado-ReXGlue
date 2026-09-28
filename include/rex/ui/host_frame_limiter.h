/**
 * @file ui/host_frame_limiter.h
 * @brief Host-presentation-only frame pacing
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace rex::ui {

// This limiter never changes guest time, vblank generation, simulation rate,
// or command contents. It only delays handing an already completed guest frame
// to the host swap chain. A zero limit leaves the path untouched.
class HostFrameLimiter {
 public:
  using Clock = std::chrono::steady_clock;

  static constexpr Clock::duration TargetInterval(uint32_t frames_per_second) {
    return frames_per_second
               ? std::chrono::duration_cast<Clock::duration>(
                     std::chrono::nanoseconds(1'000'000'000ull /
                                              frames_per_second))
               : Clock::duration::zero();
  }

  void Wait(uint32_t frames_per_second) {
    if (!frames_per_second) {
      configured_frames_per_second_ = 0;
      armed_ = false;
      return;
    }

    const Clock::duration interval = TargetInterval(frames_per_second);
    Clock::time_point now = Clock::now();
    if (!armed_ || configured_frames_per_second_ != frames_per_second) {
      configured_frames_per_second_ = frames_per_second;
      next_present_ = now + interval;
      armed_ = true;
      return;
    }

    if (now < next_present_) {
      std::this_thread::sleep_until(next_present_);
      now = Clock::now();
    }
    // Retain a stable cadence without accumulating missed-frame debt after a
    // compile, upload, or OS scheduling stall.
    do {
      next_present_ += interval;
    } while (next_present_ <= now);
  }

 private:
  uint32_t configured_frames_per_second_ = 0;
  bool armed_ = false;
  Clock::time_point next_present_{};
};

}  // namespace rex::ui
