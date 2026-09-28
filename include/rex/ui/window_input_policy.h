#pragma once

#include <rex/ui/virtual_key.h>

namespace rex::ui::window_input_policy {

inline bool ShouldToggleFullscreen(VirtualKey key, bool key_down,
                                   bool repeat, bool alt_pressed) {
  return key_down && !repeat && alt_pressed && key == VirtualKey::kReturn;
}

}  // namespace rex::ui::window_input_policy
