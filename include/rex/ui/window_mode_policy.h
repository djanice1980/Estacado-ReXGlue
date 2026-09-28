/**
 * @file ui/window_mode_policy.h
 * @brief Pure startup window-mode compatibility policy.
 */

#pragma once

#include <string_view>

namespace rex::ui::window_mode_policy {

// "auto" retains the legacy fullscreen boolean for old ReXGlue configs.
// The Darkness schema uses explicit windowed/borderless values so launchers
// never mislabel borderless desktop as exclusive fullscreen.
inline bool ShouldStartBorderless(std::string_view window_mode,
                                  bool legacy_fullscreen) {
  if (window_mode == "windowed") return false;
  if (window_mode == "borderless") return true;
  return legacy_fullscreen;
}

}  // namespace rex::ui::window_mode_policy

