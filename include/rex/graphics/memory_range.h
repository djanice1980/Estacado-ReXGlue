#pragma once

#include <cstdint>

namespace rex::graphics {

// Both ranges are half-open: [start, end). Touching endpoints don't overlap.
constexpr bool AreGuestPhysicalMemoryRangesOverlapping(uint32_t first_start,
                                                       uint32_t first_end,
                                                       uint32_t second_start,
                                                       uint32_t second_end) {
  return first_start < second_end && first_end > second_start;
}

}  // namespace rex::graphics
