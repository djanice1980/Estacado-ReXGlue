#pragma once

#include <cstdint>

#include <rex/graphics/embedded_geometry_readback_policy.h>

namespace rex::graphics::embedded_scene_resolve_capture_policy {

struct Context {
  uint64_t frame = 0, ordinal = 0, last_draw = 0;
};

enum class Kind : uint32_t { kNone = 0, kMotion = 1, kSceneColor = 2 };

struct Resolve {
  Context context;
  bool metadata_selected = false, succeeded = false, scaled = false;
  uint32_t scale_x = 1, scale_y = 1;
  uint32_t control = 0, surface = 0, color = 0, depth = 0;
  uint32_t destination_info = 0, destination_pitch = 0, destination_base = 0;
  uint32_t address = 0, bytes = 0;
};

// Observed native 720p motion and scene-color resolve signatures. No address
// selects the camera: each actual epoch is copied independently for analysis.
inline Kind Classify(const Resolve& r) noexcept {
  if (!r.metadata_selected || !r.succeeded || r.scaled || r.scale_x != 1 || r.scale_y != 1 ||
      !r.context.frame || !r.context.ordinal || !r.context.last_draw ||
      r.surface != 0x14010500 || r.depth != 0x00010000 ||
      r.destination_pitch != 0x02D00500 || !r.address || r.address != r.destination_base ||
      (r.address & 3) || (r.bytes & 3) || !r.bytes ||
      embedded_geometry_readback_policy::TextureBytes(r.address, r.bytes, 0) != r.bytes)
    return Kind::kNone;
  if (r.control == 0x00100140 && r.color == 0x00030300 && r.destination_info == 0x01000302)
    return Kind::kMotion;
  if (r.control == 0x00100040 && r.color == 0x000C0300 && r.destination_info == 0x003C0D01 &&
      !(r.bytes & 7))
    return Kind::kSceneColor;
  return Kind::kNone;
}

struct Selection {
  Kind kind = Kind::kNone;
  uint32_t copy = 0;
};

struct State {
  static constexpr uint32_t kMaximumEvents = 64;
  static constexpr uint32_t kMaximumCopiesPerKind = 2;
  uint64_t frame = 0, last_ordinal = 0, last_draw = 0, last_copy_check = 0;
  uint32_t events = 0, dropped = 0, copies = 0, motion_copies = 0, color_copies = 0;
  bool closed = false;

  bool Observe(const Context& c, bool selected) noexcept {
    if (closed) return false;
    if (frame && frame != c.frame) { closed = true; return false; }
    if (!selected || !c.frame || !c.ordinal || c.ordinal <= last_ordinal || c.last_draw < last_draw)
      return false;
    frame = c.frame;
    last_ordinal = c.ordinal;
    last_draw = c.last_draw;
    if (events == kMaximumEvents) { ++dropped; return false; }
    ++events;
    return true;
  }

  Selection SelectCopy(const Resolve& r) noexcept {
    if (closed || r.context.frame != frame || r.context.ordinal != last_ordinal ||
        r.context.last_draw != last_draw || r.context.ordinal <= last_copy_check)
      return {};
    last_copy_check = r.context.ordinal;
    const Kind kind = Classify(r);
    if (kind == Kind::kNone) return {};
    auto& count = kind == Kind::kMotion ? motion_copies : color_copies;
    if (count == kMaximumCopiesPerKind) return {};
    ++count;  // Reserve before allocation; failed GPU copies cannot retry.
    return {kind, ++copies};
  }
};

// Independent of legacy depth/texture/geometry budgets. The shared-memory
// wrapper also reserves before allocation, rejecting whole out-of-range copies.
struct RangeBudget {
  static constexpr uint32_t kMaximumCopies = 4;
  uint32_t attempts = 0;
  uint64_t bytes = 0;
  uint32_t Reserve(uint32_t address, uint64_t requested) noexcept {
    if (attempts == kMaximumCopies) return 0;
    ++attempts;
    if (!address || (address & 3) || (requested & 3)) return 0;
    const uint32_t accepted = embedded_geometry_readback_policy::TextureBytes(address, requested, 0);
    bytes += accepted;
    return accepted;
  }
};

// One host resolve-clear preparation supplies at most depth slot0 and color
// slot1. Each actual command can be reported once. The caller's existing
// selected-frame event budget bounds preparations to64 and commands to128.
struct ClearCommands {
  uint32_t target_bits = 0;
  bool Record(uint32_t slot) noexcept {
    if (slot >= 2 || (target_bits & (1u << slot))) return false;
    target_bits |= 1u << slot;
    return true;
  }
  uint32_t count() const noexcept { return (target_bits & 1) + ((target_bits >> 1) & 1); }
};

}  // namespace rex::graphics::embedded_scene_resolve_capture_policy
