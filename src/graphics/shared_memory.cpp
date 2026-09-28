/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/dbg.h>
#include <rex/graphics/gpu_diagnostics.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/shared_memory_watch_policy.h>
#include <rex/math.h>
#include <rex/memory.h>

#if REX_GPU_DIAGNOSTICS
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/hash.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(shared_memory_coherency_audit, false, "GPU/Diagnostics",
                    "Measurement builds: hash the pages the frame-end page reset re-uploads and "
                    "report any whose guest data changed without an invalidation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

namespace rex::graphics {

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::BeginTextureLifecycleDiagnostic(uint32_t physical_address_start,
                                                   uint32_t length) {
  if (!length || physical_address_start >= kBufferSize || length > kBufferSize - physical_address_start) {
    return;
  }
  uint32_t expected = 0;
  texture_lifecycle_diagnostic_length_.store(length, std::memory_order_relaxed);
  if (texture_lifecycle_diagnostic_base_plus_one_.compare_exchange_strong(
          expected, physical_address_start + 1, std::memory_order_release,
          std::memory_order_relaxed)) {
    RecordTextureLifecycleDiagnosticEvent(
        TextureLifecycleDiagnosticEventType::kRangeRegistered, physical_address_start, length);
  }
}

bool SharedMemory::TextureLifecycleDiagnosticOverlaps(uint32_t physical_address_start,
                                                       uint32_t length) const {
  if (!length) {
    return false;
  }
  const uint32_t base_plus_one =
      texture_lifecycle_diagnostic_base_plus_one_.load(std::memory_order_acquire);
  if (!base_plus_one) {
    return false;
  }
  const uint32_t base = base_plus_one - 1;
  const uint32_t tracked_length =
      texture_lifecycle_diagnostic_length_.load(std::memory_order_relaxed);
  return uint64_t(physical_address_start) < uint64_t(base) + tracked_length &&
         uint64_t(base) < uint64_t(physical_address_start) + length;
}

const uint8_t* SharedMemory::GuestPhysicalForRead(uint32_t start, uint32_t length) const {
  if (!length || start >= kBufferSize || length > kBufferSize - start) {
    return nullptr;
  }
  return memory_.TranslatePhysical<const uint8_t*>(start);
}

bool SharedMemory::CopyTextureLifecycleDiagnosticSource(uint32_t physical_address_start,
                                                        uint32_t length, void* output) const {
  if (!output || !length || !TextureLifecycleDiagnosticOverlaps(physical_address_start, length) ||
      physical_address_start >= kBufferSize || length > kBufferSize - physical_address_start) {
    return false;
  }
  const uint8_t* source = memory_.TranslatePhysical<const uint8_t*>(physical_address_start);
  if (!source) {
    return false;
  }
  std::memcpy(output, source, length);
  return true;
}

void SharedMemory::RecordTextureLifecycleDiagnosticEvent(
    TextureLifecycleDiagnosticEventType type, uint32_t physical_address_start, uint32_t length,
    uint64_t value0, uint64_t value1, bool hash_tracked_source) {
  const uint32_t base_plus_one =
      texture_lifecycle_diagnostic_base_plus_one_.load(std::memory_order_acquire);
  if (!base_plus_one) {
    return;
  }
  // A prompt may remain on-screen for an arbitrary time. Preserve ring space
  // for late writes and invalidations while still proving that the resource is
  // repeatedly selected by both binding paths.
  constexpr uint32_t kBindingEventLimit = 8;
  if (type == TextureLifecycleDiagnosticEventType::kDrawBinding &&
      texture_lifecycle_diagnostic_draw_binding_count_.fetch_add(1, std::memory_order_relaxed) >=
          kBindingEventLimit) {
    return;
  }
  if (type == TextureLifecycleDiagnosticEventType::kShaderViewSelected &&
      texture_lifecycle_diagnostic_shader_view_count_.fetch_add(1, std::memory_order_relaxed) >=
          kBindingEventLimit) {
    return;
  }
  const uint64_t sequence =
      texture_lifecycle_diagnostic_event_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (sequence > kTextureLifecycleDiagnosticEventCapacity) {
    return;
  }
  TextureLifecycleDiagnosticEvent& event = texture_lifecycle_diagnostic_events_[sequence - 1];
  event.host_microseconds = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
  event.type = type;
  event.address = physical_address_start;
  event.length = length;
  event.value0 = value0;
  event.value1 = value1;
  if (hash_tracked_source) {
    const uint32_t base = base_plus_one - 1;
    const uint32_t bytes = std::min(
        texture_lifecycle_diagnostic_length_.load(std::memory_order_relaxed), uint32_t(64 * 1024));
    const volatile uint8_t* source = memory_.TranslatePhysical<volatile uint8_t*>(base);
    auto hash_source = [source, bytes]() {
      uint32_t hash = 2166136261u;
      for (uint32_t i = 0; i < bytes; ++i) {
        hash = (hash ^ source[i]) * 16777619u;
      }
      return hash;
    };
    event.hash_first = source ? hash_source() : 0;
    event.hash_second = source ? hash_source() : 0;
  }
  event.ready_sequence.store(sequence, std::memory_order_release);
}

void SharedMemory::DumpTextureLifecycleDiagnosticEvents() {
  const char* const type_names[] = {
      "range_registered",   "prepare",          "commit_before_load",
      "shared_upload_copy_begin", "shared_upload_copy_end", "d3d12_load_begin",
      "host_texture_created", "d3d12_load_recorded", "watch_installed",
      "physical_write_notify", "invalidation_execute", "watch_invalidated",
      "descriptor_created",   "draw_binding",
      "shader_view_selected", "cpu_untile_expected", "gpu_untile_readback_queued",
      "gpu_untile_readback_ready", "host_texture_readback_ready",
  };
  uint64_t dumped = texture_lifecycle_diagnostic_dumped_count_.load(std::memory_order_relaxed);
  const uint64_t total = texture_lifecycle_diagnostic_event_count_.load(std::memory_order_acquire);
  const uint64_t available = std::min(total, uint64_t(kTextureLifecycleDiagnosticEventCapacity));
  while (dumped < available) {
    const TextureLifecycleDiagnosticEvent& event = texture_lifecycle_diagnostic_events_[dumped];
    const uint64_t expected_sequence = dumped + 1;
    if (event.ready_sequence.load(std::memory_order_acquire) != expected_sequence) {
      break;
    }
    const uint32_t type_index = uint32_t(event.type);
    const char* type_name = type_index < std::size(type_names) ? type_names[type_index] : "unknown";
    std::fprintf(stderr,
                 "REX_EMBEDDED_PROMPT_TEXTURE_TIMELINE sequence=%llu host_us=%llu type=%s "
                 "address=0x%08X length=%u hash_first=0x%08X hash_second=0x%08X stable=%u "
                 "value0=0x%016llX value1=0x%016llX\n",
                 static_cast<unsigned long long>(expected_sequence),
                 static_cast<unsigned long long>(event.host_microseconds), type_name, event.address,
                 event.length, event.hash_first, event.hash_second,
                 event.hash_first == event.hash_second ? 1u : 0u,
                 static_cast<unsigned long long>(event.value0),
                 static_cast<unsigned long long>(event.value1));
    ++dumped;
  }
  texture_lifecycle_diagnostic_dumped_count_.store(dumped, std::memory_order_release);
  if (total > kTextureLifecycleDiagnosticEventCapacity &&
      dumped == kTextureLifecycleDiagnosticEventCapacity) {
    static std::atomic<bool> overflow_reported{false};
    if (!overflow_reported.exchange(true, std::memory_order_relaxed)) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_PROMPT_TEXTURE_TIMELINE_OVERFLOW capacity=%u total=%llu "
                   "dropped=%llu\n",
                   kTextureLifecycleDiagnosticEventCapacity,
                   static_cast<unsigned long long>(total),
                   static_cast<unsigned long long>(total - kTextureLifecycleDiagnosticEventCapacity));
    }
  }
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  valid_buffer_a_.assign(num_system_page_flags_, 0);
  valid_buffer_b_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);
  active_valid_flags_.store(valid_buffer_a_.data(), std::memory_order_relaxed);
  staging_valid_flags_.store(valid_buffer_b_.data(), std::memory_order_relaxed);
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
  dirty_blocks_.store(0, std::memory_order_relaxed);

#if REX_GPU_DIAGNOSTICS
  coherency_audit_enabled_ = REXCVAR_GET(shared_memory_coherency_audit);
  if (coherency_audit_enabled_) {
    audit_tracked_valid_.assign(num_system_page_flags_, 0);
    audit_page_hash_.assign(size_t(kBufferSize >> page_size_log2_), 0);
    audit_page_invalidation_.assign(size_t(kBufferSize >> page_size_log2_), 0);
    audit_page_mismatch_.assign(size_t(kBufferSize >> page_size_log2_), 0);
    audit_page_pending_uses_.assign(size_t(kBufferSize >> page_size_log2_), 0);
    audit_pending_mismatches_.reserve(1024);
    std::fprintf(stderr, "REX_UPLOAD_COHERENCY_AUDIT enabled=1 page_bytes=%u clear_memory_page_state=%u\n",
                 1u << page_size_log2_, REXCVAR_GET(clear_memory_page_state) ? 1u : 0u);
  }
#endif

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  ReleaseTraceDownloadRanges();

  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  active_valid_flags_.store(nullptr, std::memory_order_relaxed);
  staging_valid_flags_.store(nullptr, std::memory_order_relaxed);
  valid_buffer_a_.clear();
  valid_buffer_a_.shrink_to_fit();
  valid_buffer_b_.clear();
  valid_buffer_b_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
  dirty_blocks_.store(0, std::memory_order_relaxed);
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  uint64_t* active = active_valid_flags_.load(std::memory_order_relaxed);
  uint64_t* staging = staging_valid_flags_.load(std::memory_order_relaxed);
  if (active && num_system_page_flags_) {
    std::memset(active, 0, num_system_page_flags_ * sizeof(uint64_t));
  }
  if (staging && num_system_page_flags_) {
    std::memset(staging, 0, num_system_page_flags_ * sizeof(uint64_t));
  }
  if (!system_page_flags_valid_and_gpu_written_.empty()) {
    std::memset(system_page_flags_valid_and_gpu_written_.data(), 0,
                num_system_page_flags_ * sizeof(uint64_t));
  }
#if REX_GPU_DIAGNOSTICS
  if (coherency_audit_enabled_) {
    std::fill(audit_tracked_valid_.begin(), audit_tracked_valid_.end(), uint64_t(0));
  }
#endif

  // Force a refresh on the next frame-end sync.
  dirty_blocks_.store(UINT32_MAX, std::memory_order_relaxed);
  gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
  invalidation_epoch_.fetch_add(1, std::memory_order_release);
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  if (!gpu_written_data_dirty_.load(std::memory_order_relaxed)) {
    return;
  }

  uint64_t* staging = staging_valid_flags_.load(std::memory_order_acquire);
  if (!staging || !num_system_page_flags_) {
    gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
    dirty_blocks_.store(0, std::memory_order_relaxed);
    return;
  }

  uint32_t dirty_mask = dirty_blocks_.exchange(0, std::memory_order_relaxed);
  uint32_t dirty_count = rex::bit_count(dirty_mask);
  if (dirty_count == 0 || dirty_count > 16) {
    std::memcpy(staging, system_page_flags_valid_and_gpu_written_.data(),
                num_system_page_flags_ * sizeof(uint64_t));
  } else {
    while (dirty_mask) {
      uint32_t block_index;
      rex::bit_scan_forward(dirty_mask, &block_index);
      dirty_mask &= ~(uint32_t(1) << block_index);
      uint32_t entry_offset = block_index * 64;
      if (entry_offset >= num_system_page_flags_) {
        continue;
      }
      uint32_t entry_count = std::min(uint32_t(64), num_system_page_flags_ - entry_offset);
      std::memcpy(staging + entry_offset,
                  system_page_flags_valid_and_gpu_written_.data() + entry_offset,
                  entry_count * sizeof(uint64_t));
    }
  }

  uint64_t* old_active = active_valid_flags_.exchange(staging, std::memory_order_acq_rel);
  staging_valid_flags_.store(old_active, std::memory_order_release);
  gpu_written_data_dirty_.store(false, std::memory_order_relaxed);
  invalidation_epoch_.fetch_add(1, std::memory_order_release);
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu,
                               GlobalWatchHandle already_handled_watch) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    if (!shared_memory_watch_policy::ShouldNotify(
            global_watch, already_handled_watch, invalidated_by_gpu)) continue;
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length,
                                     GlobalWatchHandle already_handled_watch) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true, already_handled_watch);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire();
    uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      if (valid_flags) {
        valid_flags[i] |= valid_bits;
      }
#if REX_GPU_DIAGNOSTICS
      if (coherency_audit_enabled_) {
        audit_tracked_valid_[i] |= valid_bits;
      }
#endif
      uint64_t old_gpu_written = system_page_flags_valid_and_gpu_written_[i];
      uint64_t new_gpu_written =
          written_by_gpu ? (old_gpu_written | valid_bits) : (old_gpu_written & ~valid_bits);
      if (new_gpu_written != old_gpu_written) {
        system_page_flags_valid_and_gpu_written_[i] = new_gpu_written;
        gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
        dirty_blocks_.fetch_or(uint32_t(1) << (i >> 6), std::memory_order_relaxed);
      }
    }
#if REX_GPU_DIAGNOSTICS
    // GPU-written pages no longer mirror guest memory: no upload to compare.
    if (coherency_audit_enabled_ && written_by_gpu) {
      std::fill(audit_page_hash_.begin() + valid_page_first,
                audit_page_hash_.begin() + valid_page_last + 1, uint64_t(0));
    }
#endif
  }

  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  // The merge buffer is reused (command processor thread only) so that the
  // per-draw vertex/index buffer requests don't allocate.
  std::vector<std::pair<uint32_t, uint32_t>>& merged_ranges = merged_ranges_scratch_;
  merged_ranges.clear();
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  // A host write made inside the plugin (command processor or XMA) before
  // this request must be visible to it: publish pending ones it touches.
  if (host_write_publisher_) {
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      if (HostWritesPendingIn(range.first, range.second)) {
        PublishHostWrites();
        break;
      }
    }
  }
#if REX_GPU_DIAGNOSTICS
  if (coherency_audit_enabled_) {
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      CoherencyAuditUsed(range.first, range.second);
    }
  }
#endif

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_acquire);
  if (valid_flags) {
    bool all_valid = true;
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      if (!range.second) {
        continue;
      }
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = valid_flags[i];
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }
        if (block_valid != UINT64_MAX) {
          all_valid = false;
          break;
        }
      }
      if (!all_valid) {
        break;
      }
    }
    if (all_valid) {
      COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
      COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                        uint32_t(merged_ranges.size()));
      COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count", 0);
      return true;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto global_lock = global_critical_region_.Acquire();
    valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = valid_flags ? valid_flags[i] : 0;
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    return true;
  }

  return UploadRanges(upload_ranges_);
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

bool SharedMemory::IsRangeResident(uint32_t start, uint32_t length) const {
  if (!length) {
    return true;
  }
  if (start >= kBufferSize || kBufferSize - start < length) {
    return false;
  }
  if (host_write_publisher_ && HostWritesPendingIn(start, length)) {
    return false;
  }
  const uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_acquire);
  if (!valid_flags) {
    return false;
  }
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  for (uint32_t block = page_first >> 6; block <= (page_last >> 6); ++block) {
    uint64_t block_valid = valid_flags[block];
    if (block == (page_first >> 6)) {
      block_valid |= (uint64_t(1) << (page_first & 63)) - 1;
    }
    if (block == (page_last >> 6) && (page_last & 63) != 63) {
      block_valid |= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
    }
    if (block_valid != UINT64_MAX) {
      return false;
    }
  }
  return true;
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  if (kGpuDiagnostics && TextureLifecycleDiagnosticOverlaps(physical_address_start, length)) {
    RecordTextureLifecycleDiagnosticEvent(
        TextureLifecycleDiagnosticEventType::kInvalidationExecute, physical_address_start, length,
        exact_range ? 1u : 0u);
  }
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  uint32_t dirty_blocks_mask = 0;
  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (valid_flags) {
      valid_flags[i] &= ~invalidate_bits;
    }
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
#if REX_GPU_DIAGNOSTICS
    if (coherency_audit_enabled_) {
      audit_tracked_valid_[i] &= ~invalidate_bits;
    }
#endif
    dirty_blocks_mask |= uint32_t(1) << (i >> 6);
  }
  gpu_written_data_dirty_.store(true, std::memory_order_relaxed);
  dirty_blocks_.fetch_or(dirty_blocks_mask, std::memory_order_relaxed);
  invalidation_epoch_.fetch_add(1, std::memory_order_release);
#if REX_GPU_DIAGNOSTICS
  if (coherency_audit_enabled_) {
    CoherencyAuditInvalidated(page_first, page_last);
  }
#endif

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

void SharedMemory::MarkHostWrite(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_last = (start + length - 1) >> 12;
  for (uint32_t page = start >> 12; page <= page_last;) {
    const uint32_t word = page >> 6;
    const uint32_t word_page_last = std::min(page_last, (word << 6) | 63);
    const uint32_t count = word_page_last - page + 1;
    const uint64_t bits =
        count == 64 ? ~uint64_t(0) : ((uint64_t(1) << count) - 1) << (page & 63);
    // Never skipped when already set: the publication's acquire exchange that
    // consumes these bits must synchronize with this writer's payload.
    host_write_pages_[word].fetch_or(bits, std::memory_order_acq_rel);
    host_write_summary_[word >> 6].fetch_or(uint64_t(1) << (word & 63),
                                            std::memory_order_acq_rel);
    page = word_page_last + 1;
  }
}

bool SharedMemory::HostWritesPendingIn(uint32_t start, uint32_t length) const {
  if (!length) {
    return false;
  }
  const uint32_t page_first = start >> 12;
  const uint32_t page_last = (start + length - 1) >> 12;
  for (uint32_t word = page_first >> 6; word <= page_last >> 6; ++word) {
    uint64_t bits = host_write_pages_[word].load(std::memory_order_relaxed);
    if (!bits) {
      continue;
    }
    if (word == page_first >> 6) {
      bits &= ~uint64_t(0) << (page_first & 63);
    }
    if (word == page_last >> 6 && (page_last & 63) != 63) {
      bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (bits) {
      return true;
    }
  }
  return false;
}

void SharedMemory::PublishHostWrites() {
  if (!host_write_publisher_) {
    return;
  }
  uint32_t range_first = UINT32_MAX;
  uint32_t range_last = 0;
  auto publish = [&]() {
    if (range_first != UINT32_MAX) {
      host_write_publisher_(host_write_publisher_context_, range_first << 12,
                            (range_last - range_first + 1) << 12);
      range_first = UINT32_MAX;
    }
  };
  for (uint32_t summary = 0; summary < host_write_summary_.size(); ++summary) {
    uint64_t words = host_write_summary_[summary].exchange(0, std::memory_order_acq_rel);
    while (words) {
      const uint32_t word = summary * 64 + rex::tzcnt(words);
      words &= words - 1;
      uint64_t pages = host_write_pages_[word].exchange(0, std::memory_order_acq_rel);
      while (pages) {
        const uint32_t page = word * 64 + rex::tzcnt(pages);
        pages &= pages - 1;
        if (range_first != UINT32_MAX && page == range_last + 1) {
          range_last = page;
        } else {
          publish();
          range_first = range_last = page;
        }
      }
    }
  }
  publish();
}

#if REX_GPU_DIAGNOSTICS
void SharedMemory::CoherencyAuditInvalidated(uint32_t page_first, uint32_t page_last) {
  // Global critical region held by the caller.
  const uint64_t sequence = ++audit_invalidation_sequence_;
  ++audit_window_.invalidations;
  std::fill(audit_page_invalidation_.begin() + page_first,
            audit_page_invalidation_.begin() + page_last + 1, sequence);
}

void SharedMemory::CoherencyAuditBeforeUpload(uint32_t page_first, uint32_t page_count) {
  audit_chunk_tracked_.resize(page_count);
  auto global_lock = global_critical_region_.Acquire();
  audit_chunk_sequence_ = audit_invalidation_sequence_;
  for (uint32_t i = 0; i < page_count; ++i) {
    const uint32_t page = page_first + i;
    audit_chunk_tracked_[i] = uint8_t((audit_tracked_valid_[page >> 6] >> (page & 63)) & 1);
  }
}

void SharedMemory::CoherencyAuditAfterUpload(uint32_t page_first, uint32_t page_count,
                                             const uint8_t* data) {
  const uint32_t page_bytes = uint32_t(1) << page_size_log2_;
  // Hash outside the lock (guest threads take it to publish their writes).
  audit_chunk_hash_.resize(page_count);
  for (uint32_t i = 0; i < page_count; ++i) {
    audit_chunk_hash_[i] = XXH3_64bits(data + size_t(i) * page_bytes, page_bytes) | 1;
  }
  auto global_lock = global_critical_region_.Acquire();
  ++audit_window_.upload_chunks;
  for (uint32_t i = 0; i < page_count; ++i) {
    const uint32_t page = page_first + i;
    uint64_t& stored = audit_page_hash_[page];
    if (audit_page_invalidation_[page] > audit_chunk_sequence_) {
      // A CPU write was published during the copy; the page is invalid again.
      ++audit_window_.racing_pages;
      stored = 0;
      continue;
    }
    const uint64_t hash = audit_chunk_hash_[i];
    const uint8_t* page_data = data + size_t(i) * page_bytes;
    auto copy = std::find_if(audit_page_copies_.begin(), audit_page_copies_.end(),
                             [page](const AuditPageCopy& entry) { return entry.page == page; });
    if (i < audit_chunk_tracked_.size() && audit_chunk_tracked_[i] && stored) {
      ++audit_window_.clear_only_pages;
      if (stored != hash) {
        ++audit_window_.mismatches;
        CoherencyAuditPushMismatch(page);
        if (copy != audit_page_copies_.end() && !copy->bytes.empty() && copy->diffs < 3) {
          ++copy->diffs;
          CoherencyAuditLogDiff(page, copy->bytes.data(), page_data);
        }
      }
    } else {
      ++audit_window_.tracked_pages;
    }
    if (copy != audit_page_copies_.end()) {
      copy->bytes.assign(page_data, page_data + page_bytes);
    }
    stored = hash;
  }
}

void SharedMemory::CoherencyAuditLogDiff(uint32_t page, const uint8_t* old_data,
                                         const uint8_t* new_data) {
  constexpr uint32_t kMaxDiffLogs = 24;
  constexpr uint32_t kMaxRuns = 8;
  if (audit_diff_logs_ >= kMaxDiffLogs) {
    return;
  }
  ++audit_diff_logs_;
  const uint32_t page_bytes = uint32_t(1) << page_size_log2_;
  uint32_t changed = 0;
  uint32_t first_changed = UINT32_MAX;
  uint32_t last_changed = 0;
  for (uint32_t i = 0; i < page_bytes; ++i) {
    if (old_data[i] != new_data[i]) {
      ++changed;
      first_changed = std::min(first_changed, i);
      last_changed = i;
    }
  }
  std::string runs;
  uint32_t run_count = 0;
  for (uint32_t i = 0; i < page_bytes && run_count < kMaxRuns;) {
    if (old_data[i] == new_data[i]) {
      ++i;
      continue;
    }
    // Report the changed 16-byte line (big-endian guest bytes).
    const uint32_t line = i & ~uint32_t(15);
    char text[128];
    size_t used = size_t(std::snprintf(text, sizeof(text), " @0x%03X:", line));
    for (uint32_t b = 0; b < 16; ++b) {
      used += size_t(std::snprintf(text + used, sizeof(text) - used, "%02X", old_data[line + b]));
    }
    used += size_t(std::snprintf(text + used, sizeof(text) - used, "->"));
    for (uint32_t b = 0; b < 16; ++b) {
      used += size_t(std::snprintf(text + used, sizeof(text) - used, "%02X", new_data[line + b]));
    }
    runs += text;
    ++run_count;
    i = line + 16;
  }
  std::string stack;
#if defined(_WIN32)
  void* frames[12] = {};
  const USHORT frame_count = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
  for (USHORT f = 0; f < frame_count; ++f) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    uintptr_t base = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<const char*>(frames[f]), &module) &&
        module) {
      base = reinterpret_cast<uintptr_t>(module);
      char path[MAX_PATH];
      if (GetModuleFileNameA(module, path, MAX_PATH)) {
        const char* slash = std::strrchr(path, '\\');
        std::snprintf(name, sizeof(name), "%s", slash ? slash + 1 : path);
      }
    }
    char entry[MAX_PATH + 32];
    std::snprintf(entry, sizeof(entry), "%s%s+0x%llX", f ? "," : "", name,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[f]) - base));
    stack += entry;
  }
#endif
  std::fprintf(stderr,
               "REX_UPLOAD_COHERENCY_DIFF page=0x%08X frame=%llu changed_bytes=%u first=0x%03X "
               "last=0x%03X lines=%s stack=%s\n",
               page << page_size_log2_, static_cast<unsigned long long>(audit_frame_), changed,
               changed ? first_changed : 0u, last_changed, runs.empty() ? " -" : runs.c_str(),
               stack.empty() ? "-" : stack.c_str());
}

void SharedMemory::CoherencyAuditPushMismatch(uint32_t page) {
  // Global critical region held by the caller.
  if (audit_pending_mismatches_.size() >= 16384) {
    return;
  }
  audit_pending_mismatches_.push_back({page, audit_invalidation_sequence_, audit_frame_});
  if (audit_page_mismatch_[page] < 255) {
    ++audit_page_mismatch_[page];
  }
}

void SharedMemory::CoherencyAuditUsed(uint32_t start, uint32_t length) {
  // Command processor thread (like every change of audit_page_mismatch_).
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  const uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_acquire);
  if (!valid_flags) {
    return;
  }
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  for (uint32_t page = start >> page_size_log2_; page <= page_last; ++page) {
    // Only a still-valid copy is used as is; an invalid page is uploaded anew.
    if (!audit_page_mismatch_[page] || !((valid_flags[page >> 6] >> (page & 63)) & 1)) {
      continue;
    }
    ++audit_window_.stale_uses;
    if (audit_page_pending_uses_[page] < UINT32_MAX) {
      ++audit_page_pending_uses_[page];
    }
  }
}

void SharedMemory::CoherencyAuditSweep() {
  // Checks what the GPU copy is trusted for, with or without the frame-end
  // reset: every page still valid since its CPU upload must equal guest
  // memory unless its invalidation is on the way (classified at frame end).
  constexpr size_t kSweepPagesPerFrame = 512;
  const uint32_t word_count = uint32_t(audit_tracked_valid_.size());
  const uint32_t page_bytes = uint32_t(1) << page_size_log2_;
  audit_sweep_pages_.clear();
  uint64_t sequence;
  {
    auto global_lock = global_critical_region_.Acquire();
    sequence = audit_invalidation_sequence_;
    for (uint32_t scanned = 0;
         scanned < word_count && audit_sweep_pages_.size() < kSweepPagesPerFrame; ++scanned) {
      const uint32_t word = audit_sweep_cursor_;
      audit_sweep_cursor_ = audit_sweep_cursor_ + 1 < word_count ? audit_sweep_cursor_ + 1 : 0;
      uint64_t bits = audit_tracked_valid_[word];
      while (bits) {
        const uint32_t page = word * 64 + rex::tzcnt(bits);
        bits &= bits - 1;
        if (audit_page_hash_[page]) {
          audit_sweep_pages_.push_back(page);
        }
      }
    }
  }
  // Hash outside the lock (guest threads take it to publish their writes).
  audit_sweep_hashes_.resize(audit_sweep_pages_.size());
  for (size_t i = 0; i < audit_sweep_pages_.size(); ++i) {
    const uint8_t* data =
        memory_.TranslatePhysical<const uint8_t*>(audit_sweep_pages_[i] << page_size_log2_);
    audit_sweep_hashes_[i] = data ? (XXH3_64bits(data, page_bytes) | 1) : 0;
  }
  auto global_lock = global_critical_region_.Acquire();
  audit_window_.sweep_pages += audit_sweep_pages_.size();
  for (size_t i = 0; i < audit_sweep_pages_.size(); ++i) {
    const uint32_t page = audit_sweep_pages_[i];
    // Invalidated (or re-uploaded) since the snapshot: nothing to compare.
    if (!audit_sweep_hashes_[i] || audit_page_invalidation_[page] > sequence ||
        !((audit_tracked_valid_[page >> 6] >> (page & 63)) & 1)) {
      continue;
    }
    const uint64_t stored = audit_page_hash_[page];
    if (!stored || stored == audit_sweep_hashes_[i]) {
      continue;
    }
    ++audit_window_.sweep_mismatches;
    ++audit_window_.mismatches;
    CoherencyAuditPushMismatch(page);
    // Take the changed content as the reference, so the page is reported
    // once per change rather than on every later sweep.
    audit_page_hash_[page] = audit_sweep_hashes_[i];
  }
}

void SharedMemory::CoherencyAuditFrameEnd() {
  if (!coherency_audit_enabled_) {
    return;
  }
  CoherencyAuditSweep();
  constexpr uint64_t kGraceFrames = 2;
  // A tracked write is published at the guest's next submission, which can
  // trail a command processor that is draining queued frames (loads): only
  // no invalidation at all for this long counts as a missed write.
  constexpr uint64_t kMissFrames = 600;
  constexpr uint64_t kWindowFrames = 300;
  auto global_lock = global_critical_region_.Acquire();
  ++audit_frame_;
  ++audit_window_.frames;
  size_t keep = 0;
  for (const AuditMismatch& mismatch : audit_pending_mismatches_) {
    const uint64_t age = audit_frame_ - mismatch.frame;
    const bool invalidated = audit_page_invalidation_[mismatch.page] > mismatch.sequence;
    if (invalidated || age >= kMissFrames) {
      // Uses of the page while this change was unexplained: harmless when the
      // change was published within the grace window (commands issued before
      // the write), suspect otherwise.
      const uint32_t pending_uses = audit_page_pending_uses_[mismatch.page];
      if (pending_uses && !(invalidated && age <= kGraceFrames)) {
        audit_window_.stale_uses_suspect += pending_uses;
        if (audit_stale_use_address_count_ < kAuditStaleUseAddressCapacity) {
          audit_stale_use_addresses_[audit_stale_use_address_count_++] =
              mismatch.page << page_size_log2_;
        }
      }
      if (audit_page_mismatch_[mismatch.page]) {
        --audit_page_mismatch_[mismatch.page];
      }
      if (!audit_page_mismatch_[mismatch.page]) {
        audit_page_pending_uses_[mismatch.page] = 0;
      }
    }
    if (invalidated && age <= kGraceFrames) {
      // The write was published after the re-upload read it: an in-flight
      // guest write, which a persistent page state would see one submission
      // boundary later, as intended.
      ++audit_window_.late_notified;
    } else if (invalidated) {
      ++audit_window_.late_slow;
      audit_window_.late_slow_max_frames = std::max(audit_window_.late_slow_max_frames, age);
    } else if (age >= kMissFrames) {
      ++audit_window_.unnotified;
      if (audit_unnotified_address_count_ < kAuditAddressCapacity) {
        audit_unnotified_addresses_[audit_unnotified_address_count_++] =
            mismatch.page << page_size_log2_;
      }
      // Keep this page's bytes from its next upload on, to log what changes.
      if (audit_page_copies_.size() < kAuditPageCopyCapacity &&
          std::none_of(audit_page_copies_.begin(), audit_page_copies_.end(),
                       [&mismatch](const AuditPageCopy& entry) {
                         return entry.page == mismatch.page;
                       })) {
        audit_page_copies_.push_back({mismatch.page, 0, {}});
      }
    } else {
      audit_pending_mismatches_[keep++] = mismatch;
    }
  }
  audit_pending_mismatches_.resize(keep);
  if (audit_window_.frames < kWindowFrames) {
    return;
  }
  const AuditCounts& w = audit_window_;
  audit_total_.frames += w.frames;
  audit_total_.upload_chunks += w.upload_chunks;
  audit_total_.tracked_pages += w.tracked_pages;
  audit_total_.clear_only_pages += w.clear_only_pages;
  audit_total_.racing_pages += w.racing_pages;
  audit_total_.mismatches += w.mismatches;
  audit_total_.late_notified += w.late_notified;
  audit_total_.unnotified += w.unnotified;
  audit_total_.invalidations += w.invalidations;
  audit_total_.sweep_pages += w.sweep_pages;
  audit_total_.sweep_mismatches += w.sweep_mismatches;
  audit_total_.late_slow += w.late_slow;
  audit_total_.stale_uses += w.stale_uses;
  audit_total_.stale_uses_suspect += w.stale_uses_suspect;
  char stale_addresses[kAuditStaleUseAddressCapacity * 11 + 1] = {};
  size_t stale_used = 0;
  for (uint32_t i = 0;
       i < audit_stale_use_address_count_ && stale_used + 12 <= sizeof(stale_addresses); ++i) {
    stale_used += size_t(std::snprintf(stale_addresses + stale_used,
                                       sizeof(stale_addresses) - stale_used, "%s0x%08X",
                                       i ? "," : "", audit_stale_use_addresses_[i]));
  }
  char addresses[kAuditAddressCapacity * 11 + 1] = {};
  size_t used = 0;
  for (uint32_t i = 0; i < audit_unnotified_address_count_ && used + 12 <= sizeof(addresses); ++i) {
    used += size_t(std::snprintf(addresses + used, sizeof(addresses) - used, "%s0x%08X",
                                 i ? "," : "", audit_unnotified_addresses_[i]));
  }
  const double frames = double(w.frames);
  std::fprintf(stderr,
               "REX_UPLOAD_COHERENCY frames=%llu chunks_per_frame=%.1f tracked_pages_per_frame=%.1f "
               "clear_only_pages_per_frame=%.1f racing_pages=%llu mismatches=%llu late_notified=%llu "
               "unnotified=%llu invalidations_per_frame=%.1f sweep_pages_per_frame=%.1f "
               "sweep_mismatches=%llu late_slow=%llu late_slow_max_frames=%llu "
               "stale_uses=%llu stale_uses_suspect=%llu stale_use_addresses=%s total_frames=%llu "
               "total_sweep_pages=%llu total_clear_only_pages=%llu total_mismatches=%llu "
               "total_late_slow=%llu total_stale_uses=%llu total_stale_uses_suspect=%llu "
               "total_unnotified=%llu unnotified_addresses=%s\n",
               static_cast<unsigned long long>(w.frames), double(w.upload_chunks) / frames,
               double(w.tracked_pages) / frames, double(w.clear_only_pages) / frames,
               static_cast<unsigned long long>(w.racing_pages),
               static_cast<unsigned long long>(w.mismatches),
               static_cast<unsigned long long>(w.late_notified),
               static_cast<unsigned long long>(w.unnotified), double(w.invalidations) / frames,
               double(w.sweep_pages) / frames,
               static_cast<unsigned long long>(w.sweep_mismatches),
               static_cast<unsigned long long>(w.late_slow),
               static_cast<unsigned long long>(w.late_slow_max_frames),
               static_cast<unsigned long long>(w.stale_uses),
               static_cast<unsigned long long>(w.stale_uses_suspect),
               stale_used ? stale_addresses : "-",
               static_cast<unsigned long long>(audit_total_.frames),
               static_cast<unsigned long long>(audit_total_.sweep_pages),
               static_cast<unsigned long long>(audit_total_.clear_only_pages),
               static_cast<unsigned long long>(audit_total_.mismatches),
               static_cast<unsigned long long>(audit_total_.late_slow),
               static_cast<unsigned long long>(audit_total_.stale_uses),
               static_cast<unsigned long long>(audit_total_.stale_uses_suspect),
               static_cast<unsigned long long>(audit_total_.unnotified),
               used ? addresses : "-");
  audit_window_ = AuditCounts();
  audit_unnotified_address_count_ = 0;
  audit_stale_use_address_count_ = 0;
}
#endif

void SharedMemory::PrepareForTraceDownload() {
  ReleaseTraceDownloadRanges();
  assert_true(trace_download_ranges_.empty());
  assert_zero(trace_download_page_count_);

  // Invalidate the entire memory CPU->GPU memory copy so all the history
  // doesn't have to be written into every frame trace, and collect the list of
  // ranges with data modified on the GPU.

  uint32_t fire_watches_range_start = UINT32_MAX;
  uint32_t gpu_written_range_start = UINT32_MAX;
  auto global_lock = global_critical_region_.Acquire();
  uint64_t* valid_flags = active_valid_flags_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    uint64_t previously_valid_block = valid_flags ? valid_flags[i] : 0;
    uint64_t gpu_written_block = system_page_flags_valid_and_gpu_written_[i];
    if (valid_flags) {
      valid_flags[i] = gpu_written_block;
    }

    // Fire watches on the invalidated pages.
    uint64_t fire_watches_block = previously_valid_block & ~gpu_written_block;
    uint64_t fire_watches_break_block = ~fire_watches_block;
    while (true) {
      uint32_t fire_watches_block_page;
      if (!rex::bit_scan_forward(fire_watches_range_start == UINT32_MAX ? fire_watches_block
                                                                        : fire_watches_break_block,
                                 &fire_watches_block_page)) {
        break;
      }
      uint32_t fire_watches_page = (i << 6) + fire_watches_block_page;
      if (fire_watches_range_start == UINT32_MAX) {
        fire_watches_range_start = fire_watches_page;
      } else {
        FireWatches(fire_watches_range_start, fire_watches_page - 1, false);
        fire_watches_range_start = UINT32_MAX;
      }
      uint64_t fire_watches_block_mask = ~((uint64_t(1) << fire_watches_block_page) - 1);
      fire_watches_block &= fire_watches_block_mask;
      fire_watches_break_block &= fire_watches_block_mask;
    }

    // Add to the GPU-written ranges.
    uint64_t gpu_written_break_block = ~gpu_written_block;
    while (true) {
      uint32_t gpu_written_block_page;
      if (!rex::bit_scan_forward(
              gpu_written_range_start == UINT32_MAX ? gpu_written_block : gpu_written_break_block,
              &gpu_written_block_page)) {
        break;
      }
      uint32_t gpu_written_page = (i << 6) + gpu_written_block_page;
      if (gpu_written_range_start == UINT32_MAX) {
        gpu_written_range_start = gpu_written_page;
      } else {
        uint32_t gpu_written_range_length = gpu_written_page - gpu_written_range_start;
        // Call EnsureHostGpuMemoryAllocated in case the page was marked as
        // GPU-written not as a result to an actual write to the shared memory
        // buffer, but, for instance, by resolving with resolution scaling (to a
        // separate buffer).
        if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                         gpu_written_range_length << page_size_log2_)) {
          trace_download_ranges_.push_back(
              std::make_pair(gpu_written_range_start << page_size_log2_,
                             gpu_written_range_length << page_size_log2_));
          trace_download_page_count_ += gpu_written_range_length;
        }
        gpu_written_range_start = UINT32_MAX;
      }
      uint64_t gpu_written_block_mask = ~((uint64_t(1) << gpu_written_block_page) - 1);
      gpu_written_block &= gpu_written_block_mask;
      gpu_written_break_block &= gpu_written_block_mask;
    }
  }
  uint32_t page_count = kBufferSize >> page_size_log2_;
  if (fire_watches_range_start != UINT32_MAX) {
    FireWatches(fire_watches_range_start, page_count - 1, false);
  }
  if (gpu_written_range_start != UINT32_MAX) {
    uint32_t gpu_written_range_length = page_count - gpu_written_range_start;
    if (EnsureHostGpuMemoryAllocated(gpu_written_range_start << page_size_log2_,
                                     gpu_written_range_length << page_size_log2_)) {
      trace_download_ranges_.push_back(std::make_pair(gpu_written_range_start << page_size_log2_,
                                                      gpu_written_range_length << page_size_log2_));
      trace_download_page_count_ += gpu_written_range_length;
    }
  }
  invalidation_epoch_.fetch_add(1, std::memory_order_release);
}

void SharedMemory::ReleaseTraceDownloadRanges() {
  trace_download_ranges_.clear();
  trace_download_ranges_.shrink_to_fit();
  trace_download_page_count_ = 0;
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  if (allocation_first == allocation_last &&
      ((host_gpu_memory_sparse_allocated_[allocation_first >> 6] >> (allocation_first & 63)) &
       1)) {
    return true;
  }
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
