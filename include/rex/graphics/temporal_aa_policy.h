#pragma once

// Temporal anti-aliasing for The Darkness (docs/NEXT_FEATURES_PLAN.md
// section 1, phase 2): the pure parts - the camera of a rendered frame, the
// majority vote that finds it, the matrix that reprojects the current frame
// into the previous one, camera-cut detection and the sub-pixel jitter.
//
// Camera (phase 1, V392-V396 captures): the world-geometry draws of the scene
// carry the camera in their vertex constants. c0..c3 are the rows of the
// rotation-only view-projection M, applied to the position relative to the
// camera: clip = M * (p - C, 1), with c0.w = c1.w = c3.w = 0 and c2.w the depth
// offset; c7.xyz = -C. Object draws fold their model transform into the same
// registers, so the camera is the constant set most depth-writing draws of
// the frame share (world geometry, ~93% on the street). Depth is reversed:
// viewport z scale -1, offset 1, so ndc_z = 1 - D.
//
// Offline check (validate_reprojection.py): reprojecting with the resolved
// depth and both frames' cameras cut the colour error of a turning frame pair
// by 79%; a still pair reprojects to 0.000 px.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rex::graphics::temporal_aa {

inline constexpr uint32_t kCameraConstantWords = 8 * 4;  // c0..c7
inline constexpr uint32_t kVoteDraws = 64;               // depth-writing draws voted per frame
inline constexpr uint32_t kVoteSlots = 8;                // distinct constant sets tracked
inline constexpr uint32_t kJitterPhases = 8;
inline constexpr double kCutDistance = 64.0;             // world units per frame
inline constexpr double kCutCosine = 0.9396926;          // 20 degrees per frame

struct Camera {
  // Row-major 4x4: rows c0..c3 (clip = M * (p - C, 1)).
  std::array<double, 16> m{};
  std::array<double, 3> position{};
  bool valid = false;
};

inline float FloatFromBits(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The camera from one draw's c0..c7 words (host-endian float bits). Rejects
// constant sets that do not have the camera layout.
inline Camera CameraFromConstants(const uint32_t* words) {
  Camera camera;
  if (!words) return camera;
  float c[kCameraConstantWords];
  for (uint32_t i = 0; i < kCameraConstantWords; ++i) {
    c[i] = FloatFromBits(words[i]);
    if (!std::isfinite(c[i])) return camera;
  }
  // Layout: c0.w = c1.w = c3.w = 0, c7.w = 0, a non-degenerate forward row.
  if (c[3] != 0.0f || c[7] != 0.0f || c[15] != 0.0f || c[31] != 0.0f) return camera;
  const double forward = std::sqrt(double(c[12]) * c[12] + double(c[13]) * c[13] +
                                   double(c[14]) * c[14]);
  if (!(forward > 0.5 && forward < 2.0)) return camera;
  for (uint32_t i = 0; i < 16; ++i) camera.m[i] = c[i];
  camera.position = {-double(c[28]), -double(c[29]), -double(c[30])};
  camera.valid = true;
  return camera;
}

// Majority vote over the first kVoteDraws depth-writing draws of a frame.
struct CameraVote {
  std::array<std::array<uint32_t, kCameraConstantWords>, kVoteSlots> sets{};
  std::array<uint32_t, kVoteSlots> counts{};
  uint32_t used = 0;
  uint32_t draws = 0;

  void Reset() { *this = CameraVote(); }
  bool Full() const { return draws >= kVoteDraws; }
  void Add(const uint32_t* words) {
    if (!words || Full()) return;
    ++draws;
    for (uint32_t i = 0; i < used; ++i) {
      if (std::memcmp(sets[i].data(), words, sizeof(sets[i])) == 0) {
        ++counts[i];
        return;
      }
    }
    if (used < kVoteSlots) {
      std::memcpy(sets[used].data(), words, sizeof(sets[used]));
      counts[used++] = 1;
    }
  }
  // The most common set that has the camera layout, if it holds a majority of
  // the voted draws.
  Camera Result() const {
    uint32_t best = kVoteSlots;
    for (uint32_t i = 0; i < used; ++i) {
      if (!CameraFromConstants(sets[i].data()).valid) continue;
      if (best == kVoteSlots || counts[i] > counts[best]) best = i;
    }
    if (best == kVoteSlots || counts[best] * 2 < draws) return {};
    return CameraFromConstants(sets[best].data());
  }
};

inline bool Invert4x4(const std::array<double, 16>& a, std::array<double, 16>& out) {
  std::array<double, 16> inv;
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
           a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
           a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
           a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
            a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
           a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
           a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
           a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
            a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
           a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
           a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
            a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
            a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
           a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
           a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
            a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
            a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (!std::isfinite(det) || std::fabs(det) < 1e-18) return false;
  for (uint32_t i = 0; i < 16; ++i) out[i] = inv[i] / det;
  return true;
}

inline std::array<double, 16> Multiply(const std::array<double, 16>& a,
                                       const std::array<double, 16>& b) {
  std::array<double, 16> r{};
  for (uint32_t row = 0; row < 4; ++row)
    for (uint32_t col = 0; col < 4; ++col)
      for (uint32_t k = 0; k < 4; ++k) r[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col];
  return r;
}

// R maps the current frame's (ndc_x, ndc_y, ndc_z, 1) to the previous frame's
// clip position (up to scale): R = M_prev * T(C_cur - C_prev) * M_cur^-1.
inline bool ReprojectionMatrix(const Camera& current, const Camera& previous,
                               std::array<double, 16>& out) {
  if (!current.valid || !previous.valid) return false;
  std::array<double, 16> inverse;
  if (!Invert4x4(current.m, inverse)) return false;
  std::array<double, 16> translate = {1, 0, 0, current.position[0] - previous.position[0],
                                      0, 1, 0, current.position[1] - previous.position[1],
                                      0, 0, 1, current.position[2] - previous.position[2],
                                      0, 0, 0, 1};
  out = Multiply(previous.m, Multiply(translate, inverse));
  for (double v : out)
    if (!std::isfinite(v)) return false;
  return true;
}

// A camera cut (teleport, cutscene cut, respawn): history must not be reused.
inline bool IsCut(const Camera& current, const Camera& previous) {
  if (!current.valid || !previous.valid) return true;
  const double dx = current.position[0] - previous.position[0];
  const double dy = current.position[1] - previous.position[1];
  const double dz = current.position[2] - previous.position[2];
  if (dx * dx + dy * dy + dz * dz > kCutDistance * kCutDistance) return true;
  // Forward rows (c3) of both frames.
  const double* a = &current.m[12];
  const double* b = &previous.m[12];
  const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  const double lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  if (la <= 0.0 || lb <= 0.0) return true;
  return (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb) < kCutCosine;
}

inline double Halton(uint32_t index, uint32_t base) {
  double result = 0.0, f = 1.0;
  for (uint32_t i = index; i; i /= base) {
    f /= double(base);
    result += f * double(i % base);
  }
  return result;
}

// Sub-pixel jitter of a frame, in pixels of the internal resolution, within
// [-0.5, 0.5): Halton (2, 3) over kJitterPhases phases (phase 0 skipped: it
// is the pixel corner of both sequences).
inline void Jitter(uint32_t frame, double& x, double& y) {
  const uint32_t phase = frame % kJitterPhases + 1;
  x = Halton(phase, 2) - 0.5;
  y = Halton(phase, 3) - 0.5;
}

}  // namespace rex::graphics::temporal_aa
