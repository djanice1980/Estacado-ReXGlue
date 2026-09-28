/**
 * @file input/mnk/mnk_state_policy.h
 * @brief Pure Xbox state publication policy for the keyboard/mouse bridge.
 */

#pragma once

#include <cstdint>

namespace rex::input::mnk::state_policy {

struct ControllerState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;

  bool operator==(const ControllerState& other) const {
    return buttons == other.buttons && left_trigger == other.left_trigger &&
           right_trigger == other.right_trigger && thumb_lx == other.thumb_lx &&
           thumb_ly == other.thumb_ly && thumb_rx == other.thumb_rx &&
           thumb_ry == other.thumb_ry;
  }
};

inline bool Publish(const ControllerState& current, ControllerState& previous,
                    bool& has_previous, uint32_t& packet_number) {
  if (has_previous && current == previous) {
    return false;
  }
  previous = current;
  has_previous = true;
  ++packet_number;
  return true;
}

}  // namespace rex::input::mnk::state_policy
