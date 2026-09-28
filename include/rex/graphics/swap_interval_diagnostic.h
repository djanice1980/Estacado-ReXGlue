#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace rex::graphics {

// CP-thread-owned observation of the existing real XE_SWAP timestamps. The
// fixed histogram is updated without allocation or per-frame logging. It
// measures completed guest-swap intervals, not display-present intervals.
struct SwapIntervalDiagnostic {
  static constexpr uint32_t kPreTextureParts = 7;
  // 512 intervals is about 8.5 s at 60 Hz, short enough to keep stationary,
  // camera-turn and walking phases apart; 512 windows cover about 73 min.
  static constexpr uint64_t kWindowSize = 512;
  static constexpr uint64_t kMaxWindows = 512;
  static constexpr uint32_t kOverflowBin = 64;
  static constexpr uint64_t kDoubledIntervalUs = 25000;
  struct WorkGroup {
    uint64_t count = 0;
    uint64_t elapsed_ticks = 0;
    uint64_t idle_ticks = 0;
    uint64_t wait_ticks = 0;
    uint64_t issue_swap_ticks = 0;
    uint64_t remaining_ticks = 0;
    uint64_t waits = 0;
    uint64_t accounting_invalid = 0;
    uint64_t thread_cpu_100ns = 0;
    uint64_t thread_cpu_samples = 0;
    // QueryThreadCycleTime deltas; GetThreadTimes above advances only on
    // scheduler ticks, so per-interval values are exact only in aggregate.
    uint64_t thread_cycles = 0;
    uint64_t thread_cycle_samples = 0;
    uint64_t draws = 0;
    uint64_t type0_packets = 0;
    uint64_t type0_words = 0;
    uint64_t zpd_calls = 0;
    uint64_t zpd_ticks = 0;
    uint64_t sampled_draws = 0;
    uint64_t sampled_draw_ticks[3]{};
    uint64_t sampled_binding_ticks = 0;
    uint64_t sampled_pre_texture_part_ticks[kPreTextureParts]{};
    uint64_t sampled_pre_texture_part_counts[kPreTextureParts]{};
    // Guest memory re-uploaded to shared memory (CPU writes since the last
    // upload) and render target updates proven identical to the previous one.
    uint64_t upload_bytes = 0;
    uint64_t upload_ranges = 0;
    uint64_t max_frame_upload_bytes = 0;
    uint64_t render_target_update_reuses = 0;
    // Time blocked on host GPU submission fences or the submission worker (a
    // subset of the remaining or issue-swap time) and full GPU
    // synchronizations (V297).
    uint64_t fence_wait_ticks = 0;
    uint64_t full_syncs = 0;
    // Host texture resource creations and the time they took (V300).
    uint64_t texture_creations = 0;
    uint64_t texture_create_ticks = 0;
  } one_refresh, doubled;
  // One observed interval, kept for per-frame attribution of long frames.
  struct FrameRecord {
    uint64_t swap = 0;
    uint64_t end_tick = 0;
    uint64_t interval_us = 0;
    uint64_t idle_ticks = 0;
    uint64_t wait_ticks = 0;
    uint64_t waits = 0;
    uint64_t issue_swap_ticks = 0;
    uint64_t fence_wait_ticks = 0;
    uint64_t full_syncs = 0;
    uint64_t texture_creations = 0;
    uint64_t texture_create_ticks = 0;
    uint64_t thread_cycles = 0;
    bool thread_cycles_valid = false;
    uint64_t draws = 0;
    uint64_t type0_words = 0;
    uint64_t upload_bytes = 0;
    uint64_t upload_ranges = 0;
  };
  struct LongFrame {
    FrameRecord previous;
    FrameRecord frame;
  };
  static constexpr uint32_t kLongFrameCapacity = 128;
  // Intervals at or above long_interval_us form the 'doubled' group (default:
  // two 60 Hz refreshes). High-refresh sessions lower it
  // (embedded_swap_long_frame_us) and capture each long frame together with
  // the frame before it; the window log writes the captured records. The
  // capture storage is owned by the caller (allocated only when capturing),
  // so the observer stays small inside the command processor.
  uint64_t long_interval_us = kDoubledIntervalUs;
  bool capture_long_frames = false;
  LongFrame* long_frames = nullptr;
  uint32_t long_frame_capacity = 0;
  uint32_t long_frame_count = 0;
  uint64_t long_frames_dropped = 0;
  FrameRecord last_frame{};
  uint32_t histogram_ms[kOverflowBin + 1]{};
  bool active = false;
  uint64_t frame_idle_ticks = 0;
  uint64_t frame_wait_ticks = 0;
  uint64_t frame_issue_swap_ticks = 0;
  uint64_t frame_waits = 0;
  uint64_t frame_draws = 0;
  uint64_t frame_type0_packets = 0;
  uint64_t frame_type0_words = 0;
  uint64_t frame_zpd_calls = 0;
  uint64_t frame_zpd_ticks = 0;
  uint64_t frame_sampled_draws = 0;
  uint64_t frame_sampled_draw_ticks[3]{};
  uint64_t frame_sampled_binding_ticks = 0;
  uint64_t frame_sampled_pre_texture_part_ticks[kPreTextureParts]{};
  uint64_t frame_sampled_pre_texture_part_counts[kPreTextureParts]{};
  uint64_t frame_upload_bytes = 0;
  uint64_t frame_upload_ranges = 0;
  uint64_t frame_render_target_update_reuses = 0;
  uint64_t frame_fence_wait_ticks = 0;
  uint64_t frame_full_syncs = 0;
  uint64_t frame_texture_creations = 0;
  uint64_t frame_texture_create_ticks = 0;
  uint64_t last_thread_cpu_100ns = 0;
  bool last_thread_cpu_valid = false;
  uint64_t last_thread_cycles = 0;
  bool last_thread_cycles_valid = false;
  // Consecutive doubled intervals: a visible dip, not only a window average.
  uint64_t doubled_run = 0;
  uint64_t max_doubled_run = 0;
  // Deferred ZPD report publication (D3D12), per window. pending_now is the
  // current ended-but-unpublished queue depth and survives window resets.
  struct DeferredReports {
    uint64_t ended = 0;
    uint64_t published = 0;
    uint64_t max_depth = 0;
    uint64_t lag_swaps[4]{};  // swaps from END to publication: 0, 1, 2, 3+
    uint64_t slot_awaits = 0;
    uint64_t write_awaits = 0;
    uint64_t index_awaits = 0;
    uint64_t blocking_awaits = 0;
  } zpd;
  uint64_t zpd_pending_now = 0;
  uint64_t count = 0;
  uint64_t first_swap = 0;
  uint64_t last_swap = 0;
  uint64_t first_tick = 0;
  uint64_t last_tick = 0;
  uint64_t sum_us = 0;
  uint64_t min_us = std::numeric_limits<uint64_t>::max();
  uint64_t max_us = 0;
  uint64_t over_16667_us = 0;
  uint64_t over_33334_us = 0;

  void AddIdle(uint64_t ticks) { if (active) frame_idle_ticks += ticks; }
  void AddWait(uint64_t ticks) {
    if (active) { frame_wait_ticks += ticks; ++frame_waits; }
  }
  void AddIssueSwap(uint64_t ticks) {
    if (active) frame_issue_swap_ticks += ticks;
  }
  void AddDraw() { if (active) ++frame_draws; }
  void AddType0(uint32_t words) {
    if (active) { ++frame_type0_packets; frame_type0_words += words; }
  }
  void AddUpload(uint64_t bytes) {
    if (active) { frame_upload_bytes += bytes; ++frame_upload_ranges; }
  }
  void AddRenderTargetUpdateReuse() { if (active) ++frame_render_target_update_reuses; }
  void AddFenceWait(uint64_t ticks) { if (active) frame_fence_wait_ticks += ticks; }
  void AddFullSync() { if (active) ++frame_full_syncs; }
  void AddTextureCreation(uint64_t ticks) {
    if (active) { ++frame_texture_creations; frame_texture_create_ticks += ticks; }
  }
  void AddZpd(uint64_t ticks) {
    if (active) { ++frame_zpd_calls; frame_zpd_ticks += ticks; }
  }
  void ZpdEnded(uint64_t depth) {
    zpd_pending_now = depth;
    if (active) {
      ++zpd.ended;
      zpd.max_depth = std::max(zpd.max_depth, depth);
    }
  }
  void ZpdPublished(uint64_t lag_swaps, uint64_t depth) {
    zpd_pending_now = depth;
    if (active) {
      ++zpd.published;
      ++zpd.lag_swaps[std::min<uint64_t>(lag_swaps, 3)];
    }
  }
  // kind: 0 same-slot BEGIN, 1 packet memory write, 2 host query index.
  void ZpdAwait(uint32_t kind, bool blocking) {
    if (!active) return;
    if (kind == 0) ++zpd.slot_awaits;
    else if (kind == 1) ++zpd.write_awaits;
    else ++zpd.index_awaits;
    zpd.blocking_awaits += blocking;
  }
  // Measure one in 64 draw attempts within an interval. The boundary moves
  // with each interval's draw count; ordinary launches never read a timer.
  bool ShouldSampleDraw() const {
    return active && frame_draws && (frame_draws & 63) == 0;
  }
  void AddSampledDraw(uint64_t pre_texture_ticks, uint64_t pre_binding_ticks,
                      uint64_t post_binding_ticks, uint64_t binding_ticks,
                      const uint64_t* pre_texture_part_ticks = nullptr,
                      uint32_t marked_pre_texture_parts = 0) {
    if (!active) return;
    ++frame_sampled_draws;
    frame_sampled_draw_ticks[0] += pre_texture_ticks;
    frame_sampled_draw_ticks[1] += pre_binding_ticks;
    frame_sampled_draw_ticks[2] += post_binding_ticks;
    frame_sampled_binding_ticks += binding_ticks;
    if (pre_texture_part_ticks) {
      for (uint32_t i = 0; i < std::min(marked_pre_texture_parts, kPreTextureParts); ++i) {
        frame_sampled_pre_texture_part_ticks[i] += pre_texture_part_ticks[i];
        ++frame_sampled_pre_texture_part_counts[i];
      }
    }
  }

  void ResetFrame() {
    frame_idle_ticks = 0;
    frame_wait_ticks = 0;
    frame_issue_swap_ticks = 0;
    frame_waits = 0;
    frame_draws = 0;
    frame_type0_packets = 0;
    frame_type0_words = 0;
    frame_zpd_calls = 0;
    frame_zpd_ticks = 0;
    frame_sampled_draws = 0;
    frame_sampled_draw_ticks[0] = frame_sampled_draw_ticks[1] =
        frame_sampled_draw_ticks[2] = 0;
    frame_sampled_binding_ticks = 0;
    for (uint32_t i = 0; i < kPreTextureParts; ++i) {
      frame_sampled_pre_texture_part_ticks[i] = 0;
      frame_sampled_pre_texture_part_counts[i] = 0;
    }
    frame_upload_bytes = 0;
    frame_upload_ranges = 0;
    frame_render_target_update_reuses = 0;
    frame_fence_wait_ticks = 0;
    frame_full_syncs = 0;
    frame_texture_creations = 0;
    frame_texture_create_ticks = 0;
  }

  void SetThreadCpuBaseline(uint64_t thread_cpu_100ns, bool valid) {
    last_thread_cpu_100ns = thread_cpu_100ns;
    last_thread_cpu_valid = valid;
  }
  void SetThreadCycleBaseline(uint64_t thread_cycles, bool valid) {
    last_thread_cycles = thread_cycles;
    last_thread_cycles_valid = valid;
  }

  // Returns true once a complete window is ready to log. The caller must
  // ResetWindow after logging, before the next observation.
  bool Observe(bool enabled, uint64_t swap_ordinal, uint64_t previous_tick,
               uint64_t current_tick, uint64_t interval_us,
               uint64_t thread_cpu_100ns = 0, bool thread_cpu_valid = false,
               uint64_t thread_cycles = 0, bool thread_cycles_valid = false) {
    if (!enabled || !active || !previous_tick || current_tick <= previous_tick ||
        swap_ordinal < 2 ||
        swap_ordinal > 1 + kWindowSize * kMaxWindows ||
        count == kWindowSize) {
      ResetFrame();
      return false;
    }
    if (!count) {
      first_swap = swap_ordinal;
      first_tick = previous_tick;
    }
    last_swap = swap_ordinal;
    last_tick = current_tick;
    ++count;
    sum_us += interval_us;
    min_us = std::min(min_us, interval_us);
    max_us = std::max(max_us, interval_us);
    over_16667_us += interval_us > 16667;
    over_33334_us += interval_us > 33334;
    ++histogram_ms[std::min<uint64_t>(interval_us / 1000, kOverflowBin)];
    const bool is_doubled = interval_us >= long_interval_us;
    WorkGroup& group = is_doubled ? doubled : one_refresh;
    doubled_run = is_doubled ? doubled_run + 1 : 0;
    max_doubled_run = std::max(max_doubled_run, doubled_run);
    const uint64_t elapsed = current_tick - previous_tick;
    const uint64_t accounted = frame_idle_ticks + frame_wait_ticks + frame_issue_swap_ticks;
    ++group.count;
    group.elapsed_ticks += elapsed;
    group.idle_ticks += frame_idle_ticks;
    group.wait_ticks += frame_wait_ticks;
    group.issue_swap_ticks += frame_issue_swap_ticks;
    group.waits += frame_waits;
    group.draws += frame_draws;
    group.type0_packets += frame_type0_packets;
    group.type0_words += frame_type0_words;
    group.zpd_calls += frame_zpd_calls;
    group.zpd_ticks += frame_zpd_ticks;
    group.sampled_draws += frame_sampled_draws;
    for (uint32_t i = 0; i < 3; ++i) {
      group.sampled_draw_ticks[i] += frame_sampled_draw_ticks[i];
    }
    group.sampled_binding_ticks += frame_sampled_binding_ticks;
    for (uint32_t i = 0; i < kPreTextureParts; ++i) {
      group.sampled_pre_texture_part_ticks[i] += frame_sampled_pre_texture_part_ticks[i];
      group.sampled_pre_texture_part_counts[i] += frame_sampled_pre_texture_part_counts[i];
    }
    group.upload_bytes += frame_upload_bytes;
    group.upload_ranges += frame_upload_ranges;
    group.max_frame_upload_bytes = std::max(group.max_frame_upload_bytes, frame_upload_bytes);
    group.render_target_update_reuses += frame_render_target_update_reuses;
    group.fence_wait_ticks += frame_fence_wait_ticks;
    group.full_syncs += frame_full_syncs;
    group.texture_creations += frame_texture_creations;
    group.texture_create_ticks += frame_texture_create_ticks;
    if (thread_cpu_valid && last_thread_cpu_valid &&
        thread_cpu_100ns >= last_thread_cpu_100ns) {
      group.thread_cpu_100ns += thread_cpu_100ns - last_thread_cpu_100ns;
      ++group.thread_cpu_samples;
    }
    SetThreadCpuBaseline(thread_cpu_100ns, thread_cpu_valid);
    FrameRecord frame{};
    if (thread_cycles_valid && last_thread_cycles_valid &&
        thread_cycles >= last_thread_cycles) {
      frame.thread_cycles = thread_cycles - last_thread_cycles;
      frame.thread_cycles_valid = true;
      group.thread_cycles += frame.thread_cycles;
      ++group.thread_cycle_samples;
    }
    SetThreadCycleBaseline(thread_cycles, thread_cycles_valid);
    if (accounted <= elapsed) {
      group.remaining_ticks += elapsed - accounted;
    } else {
      ++group.accounting_invalid;
    }
    frame.swap = swap_ordinal;
    frame.end_tick = current_tick;
    frame.interval_us = interval_us;
    frame.idle_ticks = frame_idle_ticks;
    frame.wait_ticks = frame_wait_ticks;
    frame.waits = frame_waits;
    frame.issue_swap_ticks = frame_issue_swap_ticks;
    frame.fence_wait_ticks = frame_fence_wait_ticks;
    frame.full_syncs = frame_full_syncs;
    frame.texture_creations = frame_texture_creations;
    frame.texture_create_ticks = frame_texture_create_ticks;
    frame.draws = frame_draws;
    frame.type0_words = frame_type0_words;
    frame.upload_bytes = frame_upload_bytes;
    frame.upload_ranges = frame_upload_ranges;
    if (capture_long_frames && is_doubled) {
      if (long_frame_count < long_frame_capacity) {
        long_frames[long_frame_count++] = {last_frame, frame};
      } else {
        ++long_frames_dropped;
      }
    }
    last_frame = frame;
    ResetFrame();
    return count == kWindowSize;
  }

  void ResetWindow() {
    const bool was_active = active;
    const uint64_t previous_cpu = last_thread_cpu_100ns;
    const bool previous_cpu_valid = last_thread_cpu_valid;
    const uint64_t previous_cycles = last_thread_cycles;
    const bool previous_cycles_valid = last_thread_cycles_valid;
    const uint64_t pending = zpd_pending_now;
    const uint64_t run = doubled_run;
    const uint64_t threshold = long_interval_us;
    const bool capture = capture_long_frames;
    LongFrame* const storage = long_frames;
    const uint32_t capacity = long_frame_capacity;
    const FrameRecord previous_frame = last_frame;
    *this = {};
    active = was_active;
    SetThreadCpuBaseline(previous_cpu, previous_cpu_valid);
    SetThreadCycleBaseline(previous_cycles, previous_cycles_valid);
    zpd_pending_now = pending;
    doubled_run = run;
    long_interval_us = threshold;
    capture_long_frames = capture;
    long_frames = storage;
    long_frame_capacity = capacity;
    last_frame = previous_frame;
  }
};

}  // namespace rex::graphics
