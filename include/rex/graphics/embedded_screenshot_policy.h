#pragma once

#include <cstdint>

#include <rex/ui/virtual_key.h>

namespace rex::graphics::embedded_screenshot_policy {

inline constexpr uint32_t kMaximumPendingCaptures = 4;

inline constexpr bool IsCaptureRequest(rex::ui::VirtualKey virtual_key,
                                       bool previous_key_state) {
  return virtual_key == rex::ui::VirtualKey::kF12 && !previous_key_state;
}

}  // namespace rex::graphics::embedded_screenshot_policy
