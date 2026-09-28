/**
 * @file graphics/d3d12/pipeline_preload_policy.h
 * @brief Pure policy helpers for persistent pipeline preloading.
 */

#pragma once

#include <algorithm>
#include <cstddef>

namespace rex::graphics::d3d12::pipeline_preload_policy {

inline size_t ResolveWorkerTarget(size_t existing_workers,
                                  size_t pipeline_count,
                                  size_t logical_processor_count) {
  if (!pipeline_count || !logical_processor_count) {
    return existing_workers;
  }
  const size_t preload_workers =
      std::min(pipeline_count, logical_processor_count) - size_t(1);
  return std::max(existing_workers, preload_workers);
}

inline bool MustAwaitCompletion(bool blocking, size_t workers_busy) {
  return blocking && workers_busy != 0;
}

}  // namespace rex::graphics::d3d12::pipeline_preload_policy
