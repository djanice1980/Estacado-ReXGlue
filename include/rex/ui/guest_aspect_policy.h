/**
 * @file ui/guest_aspect_policy.h
 * @brief Pure host-output aspect-ratio policy helpers.
 */

#pragma once

#include <cstdint>

namespace rex::ui::guest_aspect_policy {

struct OutputRectangle {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Fits the complete guest image inside the host surface without stretching or
// cropping. Integer rounding matches the presenter's existing nearest-pixel
// policy, while the final clamp guarantees that rounding cannot escape the
// host surface.
inline OutputRectangle ResolveContained(uint32_t surface_width, uint32_t surface_height,
                                        uint32_t aspect_width, uint32_t aspect_height) {
  OutputRectangle result;
  if (!surface_width || !surface_height || !aspect_width || !aspect_height) {
    return result;
  }

  const auto rescale = [](uint32_t value, uint32_t numerator, uint32_t denominator) {
    return uint32_t((uint64_t(value) * numerator + (denominator >> 1)) / denominator);
  };

  if (uint64_t(surface_width) * aspect_height >
      uint64_t(surface_height) * aspect_width) {
    result.height = surface_height;
    result.width = rescale(surface_height, aspect_width, aspect_height);
    if (result.width > surface_width) {
      result.width = surface_width;
      result.height = rescale(surface_width, aspect_height, aspect_width);
    }
  } else {
    result.width = surface_width;
    result.height = rescale(surface_width, aspect_height, aspect_width);
    if (result.height > surface_height) {
      result.height = surface_height;
      result.width = rescale(surface_height, aspect_width, aspect_height);
    }
  }

  result.x = (int32_t(surface_width) - int32_t(result.width)) / 2;
  result.y = (int32_t(surface_height) - int32_t(result.height)) / 2;
  return result;
}

}  // namespace rex::ui::guest_aspect_policy
