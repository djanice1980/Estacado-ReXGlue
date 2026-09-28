/**
 * @file input/mnk/mouse_motion_policy.h
 * @brief Pure relative-mouse to right-stick transform.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rex::input::mnk::mouse_motion_policy {

struct RightStick {
  int16_t x = 0;
  int16_t y = 0;
};

// Finite low-pass filtering at the guest-poll boundary. At full strength the
// output is the average of the current and previous poll. This has at most one
// poll of tail and preserves the displacement of an isolated impulse across
// the impulse and tail samples. Zero is exact identity and clears history.
class TwoSampleSmoother {
 public:
  int32_t Apply(int32_t sample, double smoothing) noexcept {
    if (smoothing == 0.0) {
      Reset();
      return sample;
    }
    if (!std::isfinite(smoothing) || smoothing < 0.0) {
      Reset();
      return 0;
    }

    const double history_weight = std::min(smoothing, 1.0) * 0.5;
    const double filtered =
        double(sample) * (1.0 - history_weight) +
        double(previous_sample_) * history_weight + rounding_residual_;
    previous_sample_ = sample;

    const double bounded = std::clamp(
        filtered, double(std::numeric_limits<int32_t>::min()),
        double(std::numeric_limits<int32_t>::max()));
    const int32_t output = static_cast<int32_t>(std::llround(bounded));
    rounding_residual_ = bounded == filtered ? filtered - double(output) : 0.0;
    return output;
  }

  void Reset() noexcept {
    previous_sample_ = 0;
    rounding_residual_ = 0.0;
  }

 private:
  int32_t previous_sample_ = 0;
  double rounding_residual_ = 0.0;
};

// Applies acceleration to one physical relative-motion event before it is
// accumulated for the next guest poll. Zero is an exact identity fast path.
// The gain grows with event magnitude and is bounded to 2x at acceleration 1,
// avoiding poll-cadence-dependent nonlinear processing of accumulated deltas.
inline int32_t AccelerateEventDelta(int32_t delta,
                                    double acceleration) noexcept {
  if (acceleration == 0.0) {
    return delta;
  }
  if (!std::isfinite(acceleration) || acceleration < 0.0) {
    return 0;
  }
  const double bounded_acceleration = std::min(acceleration, 1.0);
  const double magnitude = std::abs(double(delta));
  const double normalized_magnitude = std::min(magnitude, 32.0) / 32.0;
  const double accelerated = double(delta) *
                             (1.0 + bounded_acceleration *
                                        normalized_magnitude);
  return static_cast<int32_t>(std::llround(std::clamp(
      accelerated, double(std::numeric_limits<int32_t>::min()),
      double(std::numeric_limits<int32_t>::max()))));
}

inline int32_t AccumulateEventDelta(int32_t accumulated, int32_t delta,
                                    double acceleration) noexcept {
  const int64_t combined = int64_t(accumulated) +
                           int64_t(AccelerateEventDelta(delta, acceleration));
  return static_cast<int32_t>(std::clamp(
      combined, int64_t(std::numeric_limits<int32_t>::min()),
      int64_t(std::numeric_limits<int32_t>::max())));
}

inline int16_t ScaleAxis(int32_t delta, double sensitivity,
                         bool negate = false) noexcept {
  constexpr double kBaseScale = 200.0;
  const double value = double(delta) * (negate ? -1.0 : 1.0) * sensitivity *
                       kBaseScale;
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<int16_t>(std::clamp(
      value, double(std::numeric_limits<int16_t>::min()),
      double(std::numeric_limits<int16_t>::max())));
}

inline RightStick Translate(int32_t delta_x, int32_t delta_y,
                            double sensitivity, bool invert_y) noexcept {
  return {ScaleAxis(delta_x, sensitivity),
          ScaleAxis(delta_y, sensitivity, !invert_y)};
}

}  // namespace rex::input::mnk::mouse_motion_policy
