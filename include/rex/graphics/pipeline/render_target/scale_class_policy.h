#pragma once

#include <cstdint>

namespace rex::graphics::render_target {

// A stencil-only alias must not reduce the resolution of depth that it cannot
// modify. This is especially important when two Xenos surface descriptions
// have the same EDRAM sample pitch (for example, 1280-wide 2x MSAA and
// 640-wide 4x MSAA), but only one falls below the native-scale threshold.
// Keeping both aliases scaled lets ownership transfers preserve every depth
// sample while the stencil aspect is updated normally.
constexpr bool ShouldKeepScaledDepthForStencilOnlyAlias(
    bool initially_scale_native, bool stencil_enabled, bool depth_write_enabled,
    bool has_color_writes, bool has_matching_scaled_depth_owner,
    uint32_t scaled_pitch_pixels, uint32_t max_render_target_width) {
  return initially_scale_native && stencil_enabled && !depth_write_enabled &&
         !has_color_writes && has_matching_scaled_depth_owner &&
         scaled_pitch_pixels <= max_render_target_width;
}

}  // namespace rex::graphics::render_target
