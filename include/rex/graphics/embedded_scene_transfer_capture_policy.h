#pragma once

#include <cstdint>

namespace rex::graphics::embedded_scene_transfer_capture_policy {

struct FrameBudget;
struct UpdateContext {
  FrameBudget* budget = nullptr;
  uint64_t frame = 0, update = 0, next_draw = 0;
  uint32_t records = 0, dropped = 0, failures = 0;
  uint32_t targets = 0, owners = 0, planned = 0, commands = 0, depth_stores = 0;
  bool helper_completed = false;
  bool Record() noexcept;
  void Drop(uint32_t count) noexcept;
};

// Streaming metadata only. One selected frame, bounded updates and records;
// failures/overflow are explicit and never affect the renderer's decisions.
struct FrameBudget {
  static constexpr uint32_t kMaximumUpdates = 2048;
  static constexpr uint32_t kMaximumRecords = 16384;
  static constexpr uint32_t kMaximumRecordsPerUpdate = 64;
  uint64_t frame = 0, last_draw = 0;
  uint32_t updates = 0, dropped_updates = 0, records = 0, dropped_records = 0;
  bool closed = false;

  UpdateContext Begin(uint64_t frame_id, uint64_t next_draw, bool selected) noexcept {
    if (closed) return {};
    if (frame && frame_id != frame) { closed = true; return {}; }
    if (!selected || !frame_id || !next_draw || next_draw < last_draw) return {};
    frame = frame_id;
    last_draw = next_draw;
    if (updates == kMaximumUpdates) { ++dropped_updates; return {}; }
    return {this, frame, ++updates, next_draw};
  }
};

inline void UpdateContext::Drop(uint32_t count) noexcept {
  dropped += count;
  if (budget) budget->dropped_records += count;
}

inline bool UpdateContext::Record() noexcept {
  if (!budget) return false;
  if (records == FrameBudget::kMaximumRecordsPerUpdate ||
      budget->records == FrameBudget::kMaximumRecords) {
    Drop(1);
    return false;
  }
  ++records;
  ++budget->records;
  return true;
}

}  // namespace rex::graphics::embedded_scene_transfer_capture_policy
