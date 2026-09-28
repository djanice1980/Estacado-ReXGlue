#pragma once

#include <rex/ui/virtual_key.h>

namespace rex::graphics::embedded_debug_overlay_policy {

// The embedded title owns F3 as a host-only diagnostic key. A held key must
// not repeatedly toggle the overlay, and unrelated keys must remain available
// to the title's optional keyboard bridge.
inline bool IsToggleRequest(rex::ui::VirtualKey key, bool previous_state) {
  return key == rex::ui::VirtualKey::kF3 && !previous_state;
}

}  // namespace rex::graphics::embedded_debug_overlay_policy
