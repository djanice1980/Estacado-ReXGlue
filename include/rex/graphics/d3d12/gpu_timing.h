/**
 * GPU time per category of recorded work (diagnostic, d3d12_gpu_timing).
 *
 * A timestamp is written only where the category of the recorded GPU work
 * changes; each timestamp closes the interval of the category before it. The
 * intervals of a frame are contiguous, so they sum to the GPU frame period
 * (previous frame's last timestamp to this frame's last), with the time
 * between submissions (GPU idle or other work on the queue, such as the
 * presenter's output pass) counted as the gap category.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::graphics::d3d12 {

enum class GpuTimingCategory : uint8_t {
  kOther,
  kDraw,
  kTransfer,           // render-target ownership transfer setup before draws
  kTransferHostDepth,  // host depth stored to the EDRAM buffer for transfers
  kTransferColor,      // transfer draws into color render targets
  kTransferDepth,      // transfer draws into depth (depth values)
  kTransferStencil,    // stencil clears and per-bit stencil transfer draws
  kResolveDump,        // host render targets dumped to the EDRAM buffer
  kResolveCopy,        // resolve copies to shared / scaled-resolve memory
  kResolveClear,       // resolve clears
  kTextureLoad,        // texture untiling / conversion
  kUpload,             // shared-memory uploads
  kSwap,               // guest front buffer to the presenter
  kBarrier,
  kGap,                // between submissions
  kCount,
};

inline const char* GpuTimingCategoryName(GpuTimingCategory category) {
  static constexpr const char* kNames[size_t(GpuTimingCategory::kCount)] = {
      "other",          "draw",         "transfer",     "transfer_host_depth",
      "transfer_color", "transfer_depth", "transfer_stencil", "resolve_dump",
      "resolve_copy",   "resolve_clear", "texture_load", "upload",
      "swap",           "barrier",      "gap"};
  return size_t(category) < size_t(GpuTimingCategory::kCount) ? kNames[size_t(category)] : "invalid";
}

// Recorded-work counters reported with the timing (per recorded frame).
enum class GpuTimingCounter : uint8_t {
  kTransferColorDraws,
  kTransferColorSamples,  // destination samples covered (scaled, all MSAA samples)
  kTransferDepthDraws,
  kTransferDepthSamples,
  kTransferStencilDraws,  // one per stencil bit pass
  kTransferStencilSamples,
  kHostDepthStores,
  kHostDepthStoreSamples,
  kResolveCopies,       // resolves with a copy to memory
  kResolveCopyBytes,    // destination bytes (scaled resolves at the draw scale)
  kTextureLoads,        // texture untiling / conversion dispatches (textures)
  kTextureLoadsScaled,  // of which from scaled resolve data
  kTextureLoadBytes,    // guest bytes loaded (scaled at the draw scale)
  kUploadBatches,       // shared-memory upload batches (each a buffer transition)
  kUploadCopies,        // copy commands
  kUploadBytes,
  kCount,
};

inline const char* GpuTimingCounterName(GpuTimingCounter counter) {
  static constexpr const char* kNames[size_t(GpuTimingCounter::kCount)] = {
      "transfer_color_draws",   "transfer_color_samples",   "transfer_depth_draws",
      "transfer_depth_samples", "transfer_stencil_draws",   "transfer_stencil_samples",
      "host_depth_stores",      "host_depth_store_samples", "resolve_copies",
      "resolve_copy_bytes",     "texture_loads",            "texture_loads_scaled",
      "texture_load_bytes",     "upload_batches",           "upload_copies",
      "upload_bytes"};
  return size_t(counter) < size_t(GpuTimingCounter::kCount) ? kNames[size_t(counter)] : "invalid";
}

// Sums of completed frames between two reports.
struct GpuTimingWindow {
  uint64_t frames = 0;
  uint64_t period_ticks = 0;
  uint64_t busy_ticks = 0;
  uint64_t max_period_ticks = 0;
  uint64_t max_busy_ticks = 0;
  uint64_t marks = 0;
  uint64_t overflow = 0;
  uint64_t non_monotonic = 0;
  std::array<uint64_t, size_t(GpuTimingCategory::kCount)> category_ticks{};

  // timestamps[i] closes an interval of categories[i]. The first interval of
  // a frame starts at the previous frame's last timestamp when known.
  void AddFrame(const uint64_t* timestamps, const uint8_t* categories, uint32_t count,
                uint32_t frame_overflow, uint64_t& previous_last, bool& have_previous) {
    if (!count) {
      return;
    }
    uint64_t previous = have_previous ? previous_last : timestamps[0];
    uint64_t period = 0;
    uint64_t busy = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t timestamp = timestamps[i];
      if (timestamp < previous) {
        ++non_monotonic;
        previous = timestamp;
        continue;
      }
      const uint64_t delta = timestamp - previous;
      const size_t category = categories[i] < uint8_t(GpuTimingCategory::kCount)
                                  ? categories[i]
                                  : size_t(GpuTimingCategory::kOther);
      category_ticks[category] += delta;
      period += delta;
      if (category != size_t(GpuTimingCategory::kGap)) {
        busy += delta;
      }
      previous = timestamp;
    }
    previous_last = previous;
    have_previous = true;
    ++frames;
    period_ticks += period;
    busy_ticks += busy;
    max_period_ticks = period > max_period_ticks ? period : max_period_ticks;
    max_busy_ticks = busy > max_busy_ticks ? busy : max_busy_ticks;
    marks += count;
    overflow += frame_overflow;
  }

  void Reset() { *this = GpuTimingWindow(); }
};

}  // namespace rex::graphics::d3d12
