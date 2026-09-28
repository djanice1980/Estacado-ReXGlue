#pragma once

#include <cstdint>

namespace rex::graphics::embedded_resolve_boundary_capture_policy {

// Semantic signature of the first 1280x720 FP16 scene-feedback resolve used by
// The Darkness in a complete gameplay frame. The destination allocation moves
// between processes, so it is deliberately not part of the selector.
struct ResolveRegisters {
  uint32_t control;
  uint32_t source_color;
  uint32_t destination_info;
  uint32_t destination_pitch;
  uint32_t surface_info;
};

inline bool IsFirstSceneFeedbackResolve(const ResolveRegisters& registers) {
  return registers.control == 0x00100040 &&
         registers.source_color == 0x000C0300 &&
         registers.destination_info == 0x003C0D01 &&
         registers.destination_pitch == 0x02D00500 &&
         registers.surface_info == 0x14010500;
}

}  // namespace rex::graphics::embedded_resolve_boundary_capture_policy
