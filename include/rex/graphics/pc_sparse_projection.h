#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace rex::graphics::pc_sparse_projection {

inline float Float(uint32_t word) noexcept {
  float value;
  std::memcpy(&value, &word, sizeof(value));
  return value;
}
inline uint32_t Bits(float value) noexcept {
  uint32_t word;
  std::memcpy(&word, &value, sizeof(word));
  return word;
}

// Invert a reviewed float32 single-product row without a tolerance or guessed
// quotient. The midpoint interval includes every value rounding to the observed
// product. Expand converted endpoints by one float, then verify exact products;
// this also handles endpoint division rounding and midpoint ties conservatively.
// Only finite normal pivot products/operands and unique normal factors qualify.
inline bool UniqueFactor(const uint32_t* input, const uint32_t* output,
                         uint32_t& factor) noexcept {
  unsigned pivot = 0;
  for (unsigned i = 0; i < 3; ++i) {
    if (!std::isfinite(Float(input[i])) || !std::isfinite(Float(output[i]))) return false;
    if (std::abs(Float(input[i])) > std::abs(Float(input[pivot]))) pivot = i;
  }
  const float x = Float(input[pivot]), y = Float(output[pivot]);
  const float infinity = std::numeric_limits<float>::infinity();
  if (!std::isnormal(x) || !std::isnormal(y)) return false;
  const float before = std::nextafter(y, -infinity), after = std::nextafter(y, infinity);
  if (!std::isfinite(before) || !std::isfinite(after)) return false;
  double low = (double(before) + double(y)) * 0.5 / double(x);
  double high = (double(y) + double(after)) * 0.5 / double(x);
  if (low > high) std::swap(low, high);
  if (!std::isfinite(low) || !std::isfinite(high) ||
      std::abs(low) > std::numeric_limits<float>::max() ||
      std::abs(high) > std::numeric_limits<float>::max()) return false;
  float candidate = std::nextafter(float(low), -infinity);
  const float end = std::nextafter(float(high), infinity);
  unsigned matches = 0, visited = 0;
  while (candidate <= end) {
    if (++visited > 8 || !std::isfinite(candidate)) return false;
    bool exact = std::isnormal(candidate);
    for (unsigned i = 0; i < 3 && exact; ++i) {
      // Reject subnormal products/operands rather than assuming the GPU's FTZ
      // mode. Zero sign is significant; no canonicalization hides differences.
      const float a = Float(input[i]), b = Float(output[i]);
      if ((a != 0 && !std::isnormal(a)) || (b != 0 && !std::isnormal(b))) exact = false;
      else exact = Bits(float(double(a) * double(candidate))) == output[i];
    }
    if (exact) { factor = Bits(candidate); ++matches; }
    candidate = std::nextafter(candidate, infinity);
  }
  return matches == 1;
}

struct Projection {
  bool valid = false;
  // Native row-major projection: x/y scales, z scale, W=view Z, z offset.
  // Viewport depth conversion is separate. This is not a world-view matrix.
  std::array<uint32_t, 16> words{};
};

inline Projection Extract(const std::array<uint32_t, 36>& words) noexcept {
  Projection result;
  for (uint32_t w : words) if (!std::isfinite(Float(w))) return result;
  if (words[3] || words[7] || words[15] || words[19] ||
      words[32] || words[33] || words[34] || words[35] != 0x3F800000u ||
      !(Float(words[11]) < 0)) return result;
  for (unsigned i = 0; i < 3; ++i)
    if (words[12 + i] != words[28 + i]) return result;
  for (unsigned row = 0; row < 3; ++row)
    if (!UniqueFactor(words.data() + 20 + row * 4, words.data() + row * 4,
                      result.words[row * 5])) return {};
  if (!(Float(result.words[0]) > 0 && Float(result.words[5]) < 0 &&
        Float(result.words[10]) > 1)) return {};
  result.words[11] = 0x3F800000u;
  result.words[14] = words[11];
  result.valid = true;
  return result;
}

}  // namespace rex::graphics::pc_sparse_projection
