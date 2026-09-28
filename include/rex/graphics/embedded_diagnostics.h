#pragma once

#include <cstdint>

namespace rex::graphics::d3d12 {

// Records only timing metadata from a guest-visible real-input transition.
// This path is diagnostic-only and cannot supply or modify controller state.
void NoteEmbeddedInputTransition(int64_t host_performance_counter,
                                 int64_t host_performance_frequency,
                                 uint32_t packet_number,
                                 uint32_t buttons) noexcept;

// Requests one diagnostic-only full GPU-frame state capture. The embedded
// host calls this only for a fresh physical F12 screenshot edge. It never
// supplies guest input and has no effect unless manual capture was explicitly
// enabled at initialization.
void RequestEmbeddedGameplayCapture() noexcept;

// Optional read-only embedded export; false when manual capture is disabled.
// Only request identity crosses threads, never GPU-thread-owned frame state.
bool GetEmbeddedGameplayCaptureRequest(uint64_t& generation) noexcept;

}  // namespace rex::graphics::d3d12
