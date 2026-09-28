/**
 * @file graphics/pipeline_storage_policy.h
 * @brief Pure lifecycle policy for persistent graphics storage writers.
 */

#pragma once

namespace rex::graphics::pipeline_storage_policy {

inline bool HasSelectedWork(bool shader, bool pipeline, bool flush_shaders,
                            bool flush_pipelines) {
  return shader || pipeline || flush_shaders || flush_pipelines;
}

inline bool MayExitAfterDrain(bool shutdown_requested,
                              bool has_selected_work) {
  return shutdown_requested && !has_selected_work;
}

}  // namespace rex::graphics::pipeline_storage_policy
