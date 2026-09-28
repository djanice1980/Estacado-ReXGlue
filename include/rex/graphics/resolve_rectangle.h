#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::graphics::draw_util {

struct ResolveRectangle {
  int32_t x0;
  int32_t y0;
  int32_t x1;
  int32_t y1;

  constexpr bool empty() const { return x0 >= x1 || y0 >= y1; }
};

// Gets the pixel rectangle covered by the three CPU-provided D3D9 resolve
// vertices, applies the Xenos vertex window-offset state, and clips it to the
// already-normalized render-target scissor.
//
// PA_SC_WINDOW_OFFSET affects the resolve triangle when
// PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE is set. GetScissor applies the
// same offset independently to PA_SC_WINDOW_SCISSOR when that register allows
// it. Applying the offset to both preserves their relative placement; omitting
// it from the vertices can turn a deliberately clipped resolve band into a
// write to an unrelated EDRAM band.
inline ResolveRectangle GetResolveRectangleFromVertices(
    const int32_t vertices_fixed[6], bool vertex_window_offset_enable,
    int32_t window_offset_x, int32_t window_offset_y, int32_t scissor_left,
    int32_t scissor_top, int32_t scissor_right, int32_t scissor_bottom) {
  ResolveRectangle rectangle;
  // Top-left is inclusive: fixed .128 is covered by pixel 0, .129 isn't.
  rectangle.x0 =
      (std::min(std::min(vertices_fixed[0], vertices_fixed[2]),
                vertices_fixed[4]) +
       127) >>
      8;
  rectangle.y0 =
      (std::min(std::min(vertices_fixed[1], vertices_fixed[3]),
                vertices_fixed[5]) +
       127) >>
      8;
  // Bottom-right is exclusive.
  rectangle.x1 =
      (std::max(std::max(vertices_fixed[0], vertices_fixed[2]),
                vertices_fixed[4]) +
       127) >>
      8;
  rectangle.y1 =
      (std::max(std::max(vertices_fixed[1], vertices_fixed[3]),
                vertices_fixed[5]) +
       127) >>
      8;

  if (vertex_window_offset_enable) {
    rectangle.x0 += window_offset_x;
    rectangle.y0 += window_offset_y;
    rectangle.x1 += window_offset_x;
    rectangle.y1 += window_offset_y;
  }

  rectangle.x0 = std::clamp(rectangle.x0, scissor_left, scissor_right);
  rectangle.y0 = std::clamp(rectangle.y0, scissor_top, scissor_bottom);
  rectangle.x1 = std::clamp(rectangle.x1, scissor_left, scissor_right);
  rectangle.y1 = std::clamp(rectangle.y1, scissor_top, scissor_bottom);
  return rectangle;
}

}  // namespace rex::graphics::draw_util
