#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::graphics::embedded_texture_readback_policy {

constexpr size_t kMaximumSubresources = 48;
constexpr uint64_t kMaximumBytes = UINT64_C(256) * 1024 * 1024;

enum class ReserveResult {
  kAccepted,
  kDuplicate,
  kSubresourceLimit,
  kByteLimit,
  kInvalid,
};

struct Key {
  uint64_t resource_identity = 0;
  uint32_t array_slice = 0;
  uint64_t observation_epoch = 0;
};

struct State {
  std::array<Key, kMaximumSubresources> keys{};
  size_t key_count = 0;
  uint64_t reserved_bytes = 0;
};

// Reserves one immutable resource/slice snapshot. Repeated shader bindings in
// the captured frame must not duplicate D3D12 copies, and malformed or very
// large resources must not turn a physical one-frame capture into unbounded
// GPU/readback memory use.
// The default epoch deduplicates a whole frame. A bounded lifecycle observer
// may explicitly supply a draw epoch when the same allocation is rewritten.
// Epochs do not bypass either global capture budget.
inline ReserveResult TryReserve(State& state, uint64_t resource_identity,
                                uint32_t array_slice, uint64_t bytes,
                                uint64_t observation_epoch = 0) {
  if (!resource_identity || !bytes) {
    return ReserveResult::kInvalid;
  }
  for (size_t i = 0; i < state.key_count; ++i) {
    if (state.keys[i].resource_identity == resource_identity &&
        state.keys[i].array_slice == array_slice &&
        state.keys[i].observation_epoch == observation_epoch) {
      return ReserveResult::kDuplicate;
    }
  }
  if (state.key_count == state.keys.size()) {
    return ReserveResult::kSubresourceLimit;
  }
  if (bytes > kMaximumBytes - state.reserved_bytes) {
    return ReserveResult::kByteLimit;
  }
  state.keys[state.key_count++] = {resource_identity, array_slice,
                                  observation_epoch};
  state.reserved_bytes += bytes;
  return ReserveResult::kAccepted;
}

}  // namespace rex::graphics::embedded_texture_readback_policy
