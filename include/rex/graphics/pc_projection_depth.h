#pragma once

#include <rex/graphics/pc_sparse_projection.h>

namespace rex::graphics::pc_projection_depth {

// Projection-coordinate conversion only. A caller must separately establish
// sample provenance, homogeneous input W and the bound matrix error before
// treating the result as camera distance. No clear pixel or temporal validity
// is inferred from this descriptor.
struct Parameters {
  bool valid = false;
  double reciprocal_scale = 0, reciprocal_bias = 0;

  // q = view_Z / input_W in the reviewed projection equation. Endpoints are
  // deliberately excluded: zero is also the native clear value. Leave output
  // untouched on failure so a rejected sample cannot manufacture depth.
  bool ProjectionCoordinate(double depth, double& q) const noexcept {
    if (!valid || !std::isfinite(depth) || !(depth > 0 && depth < 1)) return false;
    const double denominator = reciprocal_scale * depth + reciprocal_bias;
    if (!std::isfinite(denominator) || denominator <= 0) return false;
    const double value = 1.0 / denominator;
    if (!std::isfinite(value) || value <= 0) return false;
    q = value;
    return true;
  }
};

inline Parameters Derive(const pc_sparse_projection::Projection& projection,
                         const std::array<uint32_t, 6>& viewport,
                         uint32_t vte, uint32_t clip) noexcept {
  // Only the reviewed native reversed-depth convention. Do not silently adapt
  // a new viewport, clipping mode, projection shape or normalized-depth range.
  constexpr std::array<uint32_t, 6> kViewport{
      0x44200000, 0x44200000, 0xC3B40000, 0x43B40000, 0xBF800000, 0x3F800000};
  if (!projection.valid || viewport != kViewport || vte != 0x43F || clip != 0x80000)
    return {};
  const auto& p = projection.words;
  for (unsigned i = 0; i < p.size(); ++i) {
    if (!std::isfinite(pc_sparse_projection::Float(p[i]))) return {};
    if (i != 0 && i != 5 && i != 10 && i != 11 && i != 14 && p[i]) return {};
  }
  const double a = pc_sparse_projection::Float(p[10]);
  const double b = pc_sparse_projection::Float(p[14]);
  if (!(pc_sparse_projection::Float(p[0]) > 0 &&
        pc_sparse_projection::Float(p[5]) < 0 && a > 1 && b < 0) ||
      p[11] != 0x3F800000) return {};
  // D = 1 - (A + B/q), hence 1/q = (-1/B)*D + (1-A)/B.
  Parameters result;
  result.reciprocal_scale = -1.0 / b;
  result.reciprocal_bias = (1.0 - a) / b;
  result.valid = std::isfinite(result.reciprocal_scale) &&
                 std::isfinite(result.reciprocal_bias) &&
                 result.reciprocal_scale > 0 && result.reciprocal_bias > 0;
  return result;
}

}  // namespace rex::graphics::pc_projection_depth
