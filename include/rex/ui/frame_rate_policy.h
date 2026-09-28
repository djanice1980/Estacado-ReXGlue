/**
 * @file ui/frame_rate_policy.h
 * @brief Player-facing frame-rate modes resolved against the display (V288)
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui {

// display.frame_rate values. One definition shared by the runtime (timing
// selection), the GPU plugin (pacing) and the settings UIs.
enum class FrameRateMode : uint8_t {
  kOriginal,     // the title's own pacing (30 FPS gameplay); restart to change
  k60,           // 60 FPS
  kHalfRefresh,  // half the display refresh (for example 72 on 144 Hz)
  kRefresh,      // the display refresh (for example 144 on 144 Hz)
  kCustom,       // display.frame_limit
  kUncapped,     // no application cap (VRR or tearing present modes)
};

inline bool ParseFrameRateMode(std::string_view value, FrameRateMode& out) {
  if (value == "original") out = FrameRateMode::kOriginal;
  else if (value == "60") out = FrameRateMode::k60;
  else if (value == "half_refresh") out = FrameRateMode::kHalfRefresh;
  else if (value == "refresh") out = FrameRateMode::kRefresh;
  else if (value == "custom") out = FrameRateMode::kCustom;
  else if (value == "uncapped") out = FrameRateMode::kUncapped;
  else return false;
  return true;
}

// Plain whole-number caps ("120", V330) are custom limits carried by the value
// itself; custom_limit keeps display.frame_limit for the "custom" mode.
inline constexpr uint32_t kMinFrameRateCap = 20;
inline constexpr uint32_t kMaxFrameRateCap = 1000;
inline bool ParseFrameRate(std::string_view value, FrameRateMode& mode, uint32_t& custom_limit) {
  if (ParseFrameRateMode(value, mode)) return true;
  if (value.empty() || value.size() > 4) return false;
  uint32_t number = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
    number = number * 10 + uint32_t(c - '0');
  }
  if (number < kMinFrameRateCap || number > kMaxFrameRateCap || value[0] == '0') return false;
  mode = FrameRateMode::kCustom;
  custom_limit = number;
  return true;
}
inline bool IsFrameRateValue(std::string_view value) {
  FrameRateMode mode;
  uint32_t limit = 0;
  return ParseFrameRate(value, mode, limit);
}

// Modes other than kOriginal run the title with the immediate presentation
// deadline and pace frames on the host; switching among them is live.
inline bool FrameRateModeUsesHostPacing(FrameRateMode mode) {
  return mode != FrameRateMode::kOriginal;
}

struct FrameRatePolicy {
  uint32_t limit_fps = 0;        // guest production cap (0 = none)
  uint32_t vsync_interval = 1;   // refreshes per frame when presenting with vsync
  double target_fps = 0.0;       // resolved target (0 = title pacing / uncapped)
  bool display_paced = false;    // vsync divisor: the display paces frames evenly
  bool uneven_without_vrr = false;  // target does not divide the refresh
};

// present_vsync: the present mode waits for vertical blank (vsync, or the VRR
// fallback). refresh_hz <= 0 means unknown. custom_limit: display.frame_limit.
inline FrameRatePolicy ResolveFrameRatePolicy(FrameRateMode mode, uint32_t custom_limit,
                                              bool present_vsync, double refresh_hz) {
  FrameRatePolicy policy{};
  double target = 0.0;
  switch (mode) {
    case FrameRateMode::kOriginal:
    case FrameRateMode::kUncapped:
      return policy;
    case FrameRateMode::k60:
      target = 60.0;
      break;
    case FrameRateMode::kHalfRefresh:
      target = refresh_hz > 0 ? refresh_hz / 2.0 : 60.0;
      break;
    case FrameRateMode::kRefresh:
      target = refresh_hz > 0 ? refresh_hz : 60.0;
      break;
    case FrameRateMode::kCustom:
      if (!custom_limit) return policy;
      target = double(custom_limit);
      break;
  }
  policy.target_fps = target;
  if (present_vsync && refresh_hz > 0) {
    const double refreshes = refresh_hz / target;
    const double whole = std::round(refreshes);
    // 2% tolerance covers fractional refresh rates (59.94, 143.98 Hz).
    if (whole >= 1.0 && whole <= 4.0 && std::fabs(refreshes - whole) <= 0.02 * whole) {
      policy.vsync_interval = uint32_t(whole);
      policy.display_paced = true;
      return policy;
    }
    policy.uneven_without_vrr = true;
  }
  policy.limit_fps = uint32_t(std::lround(target));
  return policy;
}

// Refreshes per frame when target divides the refresh (the 2% tolerance of
// ResolveFrameRatePolicy, up to 4 refreshes); 0 when it doesn't.
inline uint32_t WholeRefreshesPerFrame(double target_fps, double refresh_hz) {
  if (!(target_fps > 0.0) || !(refresh_hz > 0.0)) return 0;
  const double refreshes = refresh_hz / target_fps;
  const double whole = std::round(refreshes);
  return (whole >= 1.0 && whole <= 4.0 && std::fabs(refreshes - whole) <= 0.02 * whole)
             ? uint32_t(whole)
             : 0;
}

// The frame rate the title sustains in its heaviest scenes on a fast PC (the
// command processor bound street views): the recommendation is the highest
// whole divisor of the refresh at or below it (144 on 144 Hz, 120 on 240 Hz).
inline constexpr double kRecommendedFrameRateCeiling = 144.0;

struct FrameRateOption {
  std::string value;     // display.frame_rate value
  double fps = 0.0;      // 0: uncapped
  bool recommended = false;
  bool uneven = false;   // with VSync on a fixed-refresh display it stutters
};

// Player-facing frame rates for a display (V330): 30 (original), 60, the
// refresh's whole divisors from 50 up, common caps between them, the refresh,
// uncapped. Values that follow the display are stored as refresh or
// half_refresh so they keep working on another monitor. Unknown refresh:
// the plain symbolic list.
inline std::vector<FrameRateOption> FrameRateOptionsForDisplay(double refresh_hz) {
  std::vector<FrameRateOption> options;
  auto add = [&](std::string value, double fps) {
    for (const FrameRateOption& option : options) {
      if (std::fabs(option.fps - fps) < 0.5) return;
    }
    FrameRateOption option;
    option.value = std::move(value);
    option.fps = fps;
    options.push_back(std::move(option));
  };
  if (!(refresh_hz > 0.0)) {
    for (const char* value : {"original", "60", "half_refresh", "refresh", "uncapped"}) {
      FrameRateOption option;
      option.value = value;
      option.fps = option.value == "original" ? 30.0 : option.value == "60" ? 60.0 : 0.0;
      options.push_back(std::move(option));
    }
    return options;
  }
  // Display-following values first so equal rates keep them.
  add("refresh", refresh_hz);
  if (refresh_hz / 2.0 >= 50.0) add("half_refresh", refresh_hz / 2.0);
  add("original", 30.0);
  add("60", 60.0);
  for (const uint32_t cap : {120u, 144u}) {
    if (double(cap) < refresh_hz - 0.5) add(std::to_string(cap), double(cap));
  }
  std::sort(options.begin(), options.end(),
            [](const FrameRateOption& a, const FrameRateOption& b) { return a.fps < b.fps; });
  double best = 0.0;
  for (FrameRateOption& option : options) {
    const uint32_t refreshes = WholeRefreshesPerFrame(option.fps, refresh_hz);
    option.uneven = refreshes == 0;
    if (refreshes && option.fps <= kRecommendedFrameRateCeiling * 1.02 && option.fps > best &&
        option.value != "original") {
      best = option.fps;
    }
  }
  for (FrameRateOption& option : options) {
    option.recommended = best > 0.0 && option.fps == best;
  }
  options.push_back({"uncapped", 0.0, false, false});
  return options;
}

inline std::string FormatFrameRate(double fps) {
  char text[32];
  if (std::fabs(fps - std::round(fps)) < 0.05) {
    std::snprintf(text, sizeof(text), "%d", int(std::lround(fps)));
  } else {
    std::snprintf(text, sizeof(text), "%.1f", fps);
  }
  return text;
}

inline std::string FrameRateOptionLabel(const FrameRateOption& option) {
  if (option.value == "uncapped") return "Uncapped (with VSync Off)";
  std::string label;
  if (option.fps > 0.0) {
    label = FormatFrameRate(option.fps);
  } else if (option.value == "half_refresh") {
    label = "Half the display refresh";
  } else if (option.value == "refresh") {
    label = "Display refresh";
  } else {
    label = option.value;
  }
  if (option.value == "original") label += " (Original)";
  if (option.recommended) label += " - Recommended";
  if (option.uneven && option.fps > 0.0) label += " - may stutter without G-SYNC/FreeSync";
  return label;
}

// Menus, the title screen and loading (display.menu_frame_rate "reduced",
// V330): with VSync the largest whole divisor of the refresh at or below 60
// (48 on 144 Hz, 60 on 60/120/240 Hz); otherwise a 60 FPS cap. Never faster
// than the gameplay policy.
inline FrameRatePolicy MenuFrameRatePolicy(const FrameRatePolicy& gameplay, bool present_vsync,
                                           double refresh_hz) {
  FrameRatePolicy policy = gameplay;
  if (present_vsync && refresh_hz > 0.0) {
    uint32_t interval = uint32_t(std::ceil(refresh_hz / 60.0 - 0.02));
    interval = std::clamp(interval, 1u, 4u);
    if (gameplay.display_paced) interval = (std::max)(interval, gameplay.vsync_interval);
    policy.vsync_interval = interval;
    policy.display_paced = true;
    policy.uneven_without_vrr = false;
    policy.limit_fps = 0;
    policy.target_fps = refresh_hz / double(interval);
    if (gameplay.limit_fps && double(gameplay.limit_fps) < policy.target_fps) {
      policy.limit_fps = gameplay.limit_fps;
      policy.target_fps = double(gameplay.limit_fps);
    }
    return policy;
  }
  policy.limit_fps = gameplay.limit_fps ? (std::min)(gameplay.limit_fps, 60u) : 60u;
  policy.target_fps = double(policy.limit_fps);
  return policy;
}

}  // namespace rex::ui
