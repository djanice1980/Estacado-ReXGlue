#pragma once

#include <algorithm>
#include <cstdint>

namespace rex::graphics {
// CP-thread-owned, observational only. Sample complete inter-swap windows,
// not every Nth wait (which could systematically miss a packet family).
struct CpCadenceDiagnostic {
  // Disjoint CPU scopes inside draw preparation, nested under CP other time.
  // Units are microseconds from existing steady-clock instrumentation.
  struct DrawStage { uint64_t calls = 0, us = 0, max_us = 0; };
  DrawStage draw_stages[3]{}; // render target, pipeline, texture
  // Full IssueDraw host duration, nested within the CP interval's "other".
  // This covers early returns and copies as well as ordinary draws; it is
  // intentionally separate from the three narrower draw preparation scopes.
  struct DrawWork { uint64_t calls = 0, ticks = 0, max_ticks = 0; };
  DrawWork draw_work{};
  void DrawWorkTime(uint64_t ticks) {
    if (!active) return;
    ++draw_work.calls;
    draw_work.ticks += ticks;
    if (ticks > draw_work.max_ticks) draw_work.max_ticks = ticks;
  }
  // Exclusive packet-handler families selected only in sparse CP windows.
  // WAIT, swap, indirect wrappers and draws use existing separate scopes.
  struct PacketWork { uint64_t calls = 0, ticks = 0, max_ticks = 0; };
  PacketWork packet_work[4]{}; // type 0, type 1, type 2, other type 3
  uint64_t type0_bulk_packets = 0;
  uint64_t type0_bulk_words = 0;
  // Direct-index type-3 opcode attribution avoids per-packet logs or a hash
  // table. It is populated only inside an opt-in sparse sample.
  PacketWork type3_opcode_work[128]{};
  // ZPD query work nested within type-3 opcode 0x5B. Stage 0 is close and
  // submit at guest END; stage 1 is retirement, with stage 2 (fence check)
  // nested inside it. Stage 3 is actual fence-event waiting and stage 4 is
  // completion-resource reclamation, both nested in stage 2. These are
  // attribution scopes, not additive totals.
  PacketWork occlusion_work[5]{};
  // GPU clock duration of a sampled host ZPD segment. It is not a CPU wait
  // or a CPU/GPU clock correlation, so keep its frequency with the sample.
  PacketWork occlusion_gpu_work{};
  uint64_t occlusion_gpu_frequency = 0;
  // Correlated CPU/GPU timing is collected only with the opt-in timestamp
  // probe. These are host milliseconds, not GPU execution durations.
  uint64_t occlusion_gpu_queue_calls = 0;
  double occlusion_submit_to_begin_ms = 0;
  double occlusion_submit_to_begin_max_ms = 0;
  double occlusion_end_to_retire_ms = 0;
  double occlusion_end_to_retire_max_ms = 0;
  double occlusion_calibration_ms = 0;
  void OcclusionGpuQueueTime(double submit_to_begin_ms,
                             double end_to_retire_ms) {
    if (!active) return;
    ++occlusion_gpu_queue_calls;
    occlusion_submit_to_begin_ms += submit_to_begin_ms;
    occlusion_submit_to_begin_max_ms =
        std::max(occlusion_submit_to_begin_max_ms, submit_to_begin_ms);
    occlusion_end_to_retire_ms += end_to_retire_ms;
    occlusion_end_to_retire_max_ms =
        std::max(occlusion_end_to_retire_max_ms, end_to_retire_ms);
  }
  void OcclusionGpuTime(uint64_t ticks, uint64_t frequency) {
    if (!active || !frequency) return;
    ++occlusion_gpu_work.calls;
    occlusion_gpu_work.ticks += ticks;
    if (ticks > occlusion_gpu_work.max_ticks) occlusion_gpu_work.max_ticks = ticks;
    occlusion_gpu_frequency = frequency;
  }
  void OcclusionWorkTime(uint32_t stage, uint64_t ticks) {
    if (!active || stage >= 5) return;
    auto& work = occlusion_work[stage];
    ++work.calls;
    work.ticks += ticks;
    if (ticks > work.max_ticks) work.max_ticks = ticks;
  }
  void PacketWorkTime(uint32_t category, uint64_t ticks, uint32_t opcode = 128) {
    if (!active || category >= 4) return;
    auto& work = packet_work[category];
    ++work.calls;
    work.ticks += ticks;
    if (ticks > work.max_ticks) work.max_ticks = ticks;
    if (category == 3 && opcode < 128) {
      auto& opcode_work = type3_opcode_work[opcode];
      ++opcode_work.calls;
      opcode_work.ticks += ticks;
      if (ticks > opcode_work.max_ticks) opcode_work.max_ticks = ticks;
    }
  }
  void DrawStageTime(uint32_t stage, uint64_t us) {
    if (!active || stage >= 3) return;
    auto& s = draw_stages[stage];
    ++s.calls;
    s.us += us;
    if (us > s.max_us) s.max_us = us;
  }
  struct WaitGroup {
    uint32_t info = 0, address = 0, ref = 0, mask = 0, poll = 0;
    uint64_t count = 0, ticks = 0;
    uint64_t sleeps = 0, sleep_ticks = 0;
  };
  // Fixed capacity: never allocate or log on a packet boundary. Full packet
  // identity keeps different comparisons at the same address separate.
  WaitGroup wait_groups[8]{};
  uint32_t wait_group_count = 0;
  uint64_t overflow_waits = 0, overflow_ticks = 0;
  uint64_t overflow_sleeps = 0, overflow_sleep_ticks = 0;
  static constexpr bool ShouldSample(uint64_t ordinal) {
    return ordinal != 0 && ordinal <= 4096ull * 120 &&
           (ordinal <= 8 || ordinal % 4096 == 0);
  }
  bool active = false;
  uint64_t begin_tick = 0;
  uint64_t idle_ticks = 0;
  uint64_t wait_ticks = 0;
  uint64_t max_wait_ticks = 0;
  uint64_t waits = 0;
  uint64_t markers = 0;
  uint64_t marker_tick = 0;
  uint32_t max_wait_info = 0, max_wait_address = 0;
  uint32_t max_wait_ref = 0, max_wait_mask = 0, max_wait_poll = 0;
  uint32_t marker_address = 0, marker_value = 0;
  void Begin(bool enabled, uint64_t ordinal, uint64_t tick) {
    if (!enabled || !ShouldSample(ordinal)) {
      // Clear a completed sample once. Ordinary frames must not pay to zero
      // the per-opcode table when no diagnostic window is active.
      if (active) *this = {};
      return;
    }
    *this = {};
    active = true;
    begin_tick = tick;
  }
  void Wait(uint64_t ticks, uint32_t info, uint32_t address,
            uint32_t ref, uint32_t mask, uint32_t poll,
            uint64_t sleeps = 0, uint64_t sleep_ticks = 0) {
    if (!active) return;
    ++waits;
    wait_ticks += ticks;
    uint32_t group = 0;
    for (; group < wait_group_count; ++group) {
      const auto& g = wait_groups[group];
      if (g.info == info && g.address == address && g.ref == ref &&
          g.mask == mask && g.poll == poll) break;
    }
    if (group < 8) {
      if (group == wait_group_count) {
        wait_groups[group] = {info, address, ref, mask, poll, 0, 0};
        ++wait_group_count;
      }
      ++wait_groups[group].count;
      wait_groups[group].ticks += ticks;
      wait_groups[group].sleeps += sleeps;
      wait_groups[group].sleep_ticks += sleep_ticks;
    } else {
      ++overflow_waits;
      overflow_ticks += ticks;
      overflow_sleeps += sleeps;
      overflow_sleep_ticks += sleep_ticks;
    }
    if (ticks >= max_wait_ticks) {
      max_wait_ticks = ticks;
      max_wait_info = info;
      max_wait_address = address;
      max_wait_ref = ref;
      max_wait_mask = mask;
      max_wait_poll = poll;
    }
  }
};
}  // namespace rex::graphics
