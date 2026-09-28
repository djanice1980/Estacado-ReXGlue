#pragma once
#include <cstdint>
#include <rex/graphics/pc_command_execution.h>
#include <rex/graphics/pc_owned_camera_packet.h>

namespace rex::graphics::pc_constant_writer {

// An observed register write, not an object, camera or allocation identity.
struct Record {
  uint64_t sequence = 0;
  uint32_t value = 0;
  uint32_t physical_address = UINT32_MAX;
  bool bulk = false;
  uint32_t packet_physical = UINT32_MAX;
  pc_command_execution::Token execution{};
  pc_owned_camera_packet::Source camera_source{};
};

inline uint32_t PhysicalWord(uintptr_t address, uintptr_t base,
                             uint32_t bytes) noexcept {
  if (address < base || bytes < 4) return UINT32_MAX;
  const uintptr_t offset = address - base;
  return offset <= bytes - 4 && !(offset & 3) ? uint32_t(offset) : UINT32_MAX;
}

inline Record Matching(const Record& observed, uint32_t bound_value) noexcept {
  return observed.sequence && observed.value == bound_value ? observed : Record{};
}

}  // namespace rex::graphics::pc_constant_writer
