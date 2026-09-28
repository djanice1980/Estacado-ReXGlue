#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include <rex/graphics/swap_interval_diagnostic.h>

namespace rex::graphics {

// Optional CP-thread-only ownership of existing interval counters. Segments are
// exclusive: entering a child closes its parent's segment, and a real XE_SWAP
// closes the current segment before the interval counters reset. Buffer IDs
// identify executions, not guest allocations or render passes. No guest writes,
// clocks, allocation, or per-draw work are added by this observer.
struct SwapBufferWorkDiagnostic {
  static constexpr uint32_t kMaxDepth = 64;
  static constexpr uint32_t kMaxSegments = 256;
  struct Owner {
    uint64_t execution = 0, parent = 0;
    uint32_t address = 0, words = 0, kind = 0, depth = 0;
    uint32_t offset = 0;
  };
  struct Counts {
    uint64_t draws = 0, packets = 0, words = 0, waits = 0, zpd = 0;
    static Counts Read(const SwapIntervalDiagnostic& interval) {
      return {interval.frame_draws, interval.frame_type0_packets,
              interval.frame_type0_words, interval.frame_wait_ticks,
              interval.frame_zpd_calls};
    }
    Counts operator-(const Counts& before) const {
      return {draws - before.draws, packets - before.packets,
              words - before.words, waits - before.waits, zpd - before.zpd};
    }
  };
  struct Segment {
    Owner owner{};
    uint32_t end_offset = 0;
    Counts work{};
  };
  struct Sample {
    uint64_t swap = 0, elapsed_us = 0;
    Counts total{};
    uint32_t count = 0, overflow = 0;
    std::array<Segment, kMaxSegments> segments{};
  };

  bool enabled = false;
  bool skip_after_dump = false;
  uint64_t sequence = 0;
  uint32_t depth = 0, untracked_depth = 0;
  std::array<Owner, kMaxDepth> stack{};
  Counts baseline{};
  uint32_t count = 0, overflow = 0;
  std::array<Segment, kMaxSegments> segments{};
  std::array<Sample, 2> samples{};

  // Called once at a root-buffer entry. Configuration remains unchanged during
  // execution; disabling cannot leave stale stack entries in a later capture.
  void SetEnabled(bool value) {
    if (enabled == value) return;
    enabled = value;
    depth = untracked_depth = count = overflow = 0;
    baseline = {};
    ResetWindow();
  }
  void Begin(Owner owner) {
    if (!enabled) return;
    if (untracked_depth || depth == kMaxDepth ||
        sequence == std::numeric_limits<uint64_t>::max()) {
      ++untracked_depth;
      return;
    }
    owner.execution = ++sequence;
    owner.parent = depth ? stack[depth - 1].execution : 0;
    owner.depth = depth + 1;
    stack[depth++] = owner;
  }
  bool Capturing(const SwapIntervalDiagnostic& interval) const {
    return enabled && interval.active && (!samples[0].swap || !samples[1].swap);
  }
  void CloseSegment(const SwapIntervalDiagnostic& interval, uint32_t offset) {
    if (!enabled) return;
    const Counts now = Counts::Read(interval);
    if (Capturing(interval)) {
      if (untracked_depth || !depth || count == kMaxSegments) {
        ++overflow;
      } else {
        segments[count++] = {stack[depth - 1], offset, now - baseline};
      }
    }
    baseline = now;
    if (!untracked_depth && depth) stack[depth - 1].offset = offset;
  }
  void End(const SwapIntervalDiagnostic& interval, uint32_t offset) {
    if (!enabled) return;
    CloseSegment(interval, offset);
    if (untracked_depth) --untracked_depth;
    else if (depth) --depth;
  }
  // CloseSegment must precede this at the actual swap packet. The interval's
  // previous-tick validity and lifetime bounds must match Observe's caller.
  void FinishFrame(const SwapIntervalDiagnostic& interval, uint64_t swap,
                   uint64_t elapsed_us, bool valid) {
    if (valid && !skip_after_dump && Capturing(interval)) {
      auto& sample = samples[elapsed_us >= interval.kDoubledIntervalUs ? 1 : 0];
      if (!sample.swap) {
        sample.swap = swap;
        sample.elapsed_us = elapsed_us;
        sample.total = Counts::Read(interval);
        sample.count = count;
        sample.overflow = overflow;
        for (uint32_t i = 0; i < count; ++i) sample.segments[i] = segments[i];
      }
    }
    count = overflow = 0;
    baseline = {};
    skip_after_dump = false;
  }
  void ResetWindow() {
    // Retain the current execution stack across a window/swap inside a buffer.
    // Stale array entries are inaccessible once their logical count is zero.
    for (auto& sample : samples) {
      sample.swap = sample.elapsed_us = 0;
      sample.count = sample.overflow = 0;
      sample.total = {};
    }
    // Do not select the interval containing the preceding window's disk dump
    // as a representative long frame. Aggregate cadence still includes it and
    // must not be presented as an ordinary-player performance measurement.
    skip_after_dump = true;
  }
};

}  // namespace rex::graphics
