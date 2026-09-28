#pragma once
#include <cstdint>
#include <type_traits>

namespace rex::graphics::pc_owned_camera_packet {
// Owned CPU producer data transported with an exact native packet copy. This
// is not a persistent camera identity or a previous-rendered-frame transform.
struct Source {
  uint64_t packet = 0, constant = 0, publication = 0;
  uint32_t item = 0, first_register = 0, source_word = 0, words = 0;
  uint32_t camera_current[16]{};
  bool Covers(uint32_t index) const noexcept {
    return packet && constant && publication && words && words <= 32 &&
        source_word < 32 && words <= 32 - source_word &&
        first_register == 0x4030u + source_word && index >= first_register &&
        uint64_t(index) < uint64_t(first_register) + words;
  }
};
static_assert(std::is_standard_layout_v<Source> && sizeof(Source) == 104);
struct Callbacks {
  void* context = nullptr;
  // On success, all count host-endian payload words and source are owned copies
  // from ONE guarded read. No lock survives return; no GPU call/wait in callback.
  // On failure, the caller must ignore both outputs and use ordinary execution.
  bool (*copy)(void*, uint32_t physical, uint32_t header, uint32_t count,
               uint32_t* payload, Source* source) noexcept = nullptr;
};
inline bool CopyContiguous(const Callbacks& callbacks, uint32_t physical,
    uint32_t header, uint32_t count, uint32_t read_offset, uint32_t capacity,
    uint32_t* payload, Source* source) noexcept {
  const uint32_t first = header & 0x7FFFu;
  if (!callbacks.copy || !payload || !source || !count || count > 1024 ||
      (header & 0xC0008000u) || ((header >> 16) & 0x3FFFu) + 1 != count ||
      first >= 0x4050 || first + count <= 0x4030 || read_offset < 4 ||
      read_offset > capacity || count * 4 > capacity - read_offset ||
      (physical & 3) || uint64_t(physical) + uint64_t(count + 1) * 4 > 0x20000000ull)
    return false;
  return callbacks.copy(callbacks.context, physical, header, count, payload, source);
}
} // namespace rex::graphics::pc_owned_camera_packet
