/**
 * @file ui/guest_frame_limiter.h
 * @brief Guest frame-production pacing (V285)
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::ui {

// Deadline policy for pacing guest frame PRODUCTION at the title's frame
// boundary (the producer thread), instead of delaying already-built frames at
// presentation. Ticks are any monotonic unit (QPC in production, synthetic in
// tests). The policy never changes guest time, VBlank, simulation rate or
// command contents; it only delays when the producer starts its next frame.
//
//  - The first frame after (re)arming never waits.
//  - Deadlines lie on a fixed grid (period apart), so the average rate is
//    exact and independent of timer granularity.
//  - A frame that finishes late by up to one period keeps the grid (the next
//    slot is shorter but never zero-length queued work).
//  - Behind by more than one period (loading, hitch): the grid restarts from
//    the current frame. There are no catch-up bursts.
class GuestFrameDeadline {
 public:
  // Returns the tick the caller must wait until before producing this frame
  // (a value <= now means no wait).
  uint64_t Begin(uint64_t now, uint64_t period) {
    if (!period) {
      armed_ = false;
      return now;
    }
    if (!armed_ || period != period_) {
      armed_ = true;
      period_ = period;
      next_ = now + period;
      return now;
    }
    const uint64_t deadline = next_;
    const uint64_t start = std::max(now, deadline);
    next_ = deadline + period;
    if (start > next_) {
      next_ = start + period;
      ++grid_resets_;
    }
    return deadline;
  }

  void Reset() { armed_ = false; }
  bool armed() const { return armed_; }
  uint64_t next_deadline() const { return next_; }
  uint64_t grid_resets() const { return grid_resets_; }

 private:
  bool armed_ = false;
  uint64_t period_ = 0;
  uint64_t next_ = 0;
  uint64_t grid_resets_ = 0;
};

// Adaptive tail for "sleep on a high-resolution timer, then spin briefly".
// The margin follows the observed timer lateness (fast rise, slow decay) so
// the spin stays short while deadlines are still met. Units: ticks.
class GuestFrameSpinMargin {
 public:
  GuestFrameSpinMargin(uint64_t minimum, uint64_t maximum, uint64_t initial)
      : minimum_(minimum), maximum_(maximum), margin_(initial) {}

  uint64_t margin() const { return margin_; }

  // lateness = how far past the requested timer expiry the thread woke.
  void Observe(uint64_t lateness, uint64_t headroom) {
    const uint64_t wanted = lateness + headroom;
    if (wanted > margin_) {
      margin_ = wanted;
    } else {
      margin_ -= (margin_ - wanted) / 16;  // slow decay
    }
    margin_ = std::clamp(margin_, minimum_, maximum_);
  }

 private:
  uint64_t minimum_;
  uint64_t maximum_;
  uint64_t margin_;
};

// Process-wide handshake: once the title reports guest frame boundaries, the
// presenter's legacy presentation-side limiter stands down so frames are
// never paced twice. Defined in rexruntime (host_frame_limiter.cpp).
void SetGuestFrameLimiterActive(bool active);
bool GuestFrameLimiterActive();

// Live developer override of the host present mode (-1 = use the configured
// display_present_mode; otherwise a HostPresentMode value) and of the vsync
// interval (0 = use display_vsync_interval). Test launches only.
void SetHostPresentOverride(int32_t present_mode, uint32_t vsync_interval);
int32_t HostPresentModeOverride();
uint32_t HostVsyncIntervalOverride();

}  // namespace rex::ui
