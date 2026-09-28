#pragma once
#include <algorithm>
#include <cstdint>

namespace rex::graphics::embedded_geometry_readback_policy {
constexpr uint32_t kMaximumCopies = 36;  // four pairs, eight streams + indices
constexpr uint32_t kMaximumBytes = 4096;
constexpr uint32_t kMemoryBytes = 512u * 1024u * 1024u;
inline uint32_t CaptureBytes(uint32_t address, uint64_t requested, uint32_t copies) {
  if (copies >= kMaximumCopies || address >= kMemoryBytes || !requested) return 0;
  return uint32_t(std::min({requested, uint64_t(kMaximumBytes), uint64_t(kMemoryBytes-address)}));
}
inline bool MayMap(uint64_t submission, uint64_t completed) {
  return submission != 0 && completed >= submission;
}
inline uint32_t ContentHash(const uint8_t* bytes, uint32_t count) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}
// Separate exact texture-source budget. Never accept a truncated texture as a
// complete input representation (unlike bounded vertex previews above).
inline uint32_t TextureBytes(uint32_t address, uint64_t requested, uint32_t copies) {
  constexpr uint64_t kTextureMaximumBytes = 8u * 1024u * 1024u;
  if (copies >= 8 || !requested || requested > kTextureMaximumBytes ||
      address >= kMemoryBytes || requested > uint64_t(kMemoryBytes - address)) return 0;
  return uint32_t(requested);
}
}  // namespace rex::graphics::embedded_geometry_readback_policy
