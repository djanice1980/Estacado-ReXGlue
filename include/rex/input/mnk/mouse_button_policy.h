/**
 * @file input/mnk/mouse_button_policy.h
 * @brief Pure physical mouse-button to virtual-key mapping.
 */

#pragma once

#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

namespace rex::input::mnk::mouse_button_policy {

inline rex::ui::VirtualKey ToVirtualKey(rex::ui::MouseEvent::Button button) {
  using Button = rex::ui::MouseEvent::Button;
  using Key = rex::ui::VirtualKey;
  switch (button) {
    case Button::kLeft:
      return Key::kLButton;
    case Button::kRight:
      return Key::kRButton;
    case Button::kMiddle:
      return Key::kMButton;
    case Button::kX1:
      return Key::kXButton1;
    case Button::kX2:
      return Key::kXButton2;
    default:
      return Key::kNone;
  }
}

}  // namespace rex::input::mnk::mouse_button_policy
