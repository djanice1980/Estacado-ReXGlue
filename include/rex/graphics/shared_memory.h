#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/memory.h>
#include <rex/thread/mutex.h>

// Set by the rexgpu-xenos build (REXGLUE_GPU_DIAGNOSTICS); player builds: 0.
#ifndef REX_GPU_DIAGNOSTICS
#define REX_GPU_DIAGNOSTICS 0
#endif

namespace rex::graphics {

// Manages memory for unconverted textures, resolve targets, vertex and index
// buffers that can be accessed from shaders with Xenon physical addresses, with
// system page size granularity.
class SharedMemory {
 public:
  static constexpr uint32_t kBufferSizeLog2 = 29;
  static constexpr uint32_t kBufferSize = 1 << kBufferSizeLog2;

  virtual ~SharedMemory();
  // Call in the implementation-specific ClearCache.
  virtual void ClearCache();
  void SetSystemPageBlocksValidWithGpuDataWritten();
  void InvalidateAllPages();

#if REX_GPU_DIAGNOSTICS
  // Upload coherency audit (measurement builds, shared_memory_coherency_audit):
  // checks that the frame-end page reset (clear_memory_page_state) only
  // re-uploads pages whose guest data is unchanged, i.e. that every CPU write
  // reached MemoryInvalidationCallback. Call once per guest frame (command
  // processor thread); logs REX_UPLOAD_COHERENCY windows.
  void CoherencyAuditFrameEnd();
#endif

  typedef void (*GlobalWatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                      void* context, uint32_t address_first, uint32_t address_last,
                                      bool invalidated_by_gpu);
  typedef void* GlobalWatchHandle;
  // Registers a callback invoked when something is invalidated in the GPU
  // memory copy by the CPU or (if triggered explicitly - such as by a resolve)
  // by the GPU. It will be fired for writes to pages previously requested, but
  // may also be fired regardless of whether it was used by GPU emulation - for
  // example, if the game changes protection level of a memory range containing
  // the watched range.
  //
  // The callback is called within the global critical region.
  GlobalWatchHandle RegisterGlobalWatch(GlobalWatchCallback callback, void* callback_context);
  void UnregisterGlobalWatch(GlobalWatchHandle handle);
  typedef void (*WatchCallback)(const std::unique_lock<std::recursive_mutex>& global_lock,
                                void* context, void* data, uint64_t argument,
                                bool invalidated_by_gpu);
  typedef void* WatchHandle;
  // Registers a callback invoked when the specified memory range is invalidated
  // in the GPU memory copy by the CPU or (if triggered explicitly - such as by
  // a resolve) by the GPU. It will be fired for writes to pages previously
  // requested, but may also be fired regardless of whether it was used by GPU
  // emulation - for example, if the game changes protection level of a memory
  // range containing the watched range.
  //
  // Generally the context is the subsystem pointer (for example, the texture
  // cache), the data is the object (such as a texture), and the argument is
  // additional subsystem/object-specific data (such as whether the range
  // belongs to the base mip level or to the rest of the mips).
  //
  // Called with the global critical region locked. Do NOT watch or unwatch
  // ranges from within it! The watch for the callback is cancelled after the
  // callback - the handle becomes invalid.
  WatchHandle WatchMemoryRange(uint32_t start, uint32_t length, WatchCallback callback,
                               void* callback_context, void* callback_data,
                               uint64_t callback_argument);
  // Unregisters previously registered watched memory range.
  void UnwatchMemoryRange(WatchHandle handle);

  // Checks if the range has been updated, uploads new data if needed and
  // ensures the host GPU memory backing the range are resident. Returns true if
  // the range has been fully updated and is usable.
  bool RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count);
  bool RequestRange(uint32_t start, uint32_t length);

  // For per-draw residency caches (command processor thread): lock-free, like
  // the fast path of RequestRanges - every page of the range holds a valid GPU
  // copy and no plugin host write to it is pending. When false, request it.
  bool IsRangeResident(uint32_t start, uint32_t length) const;
  // Read-only view of guest physical memory (texture pack content ids).
  // nullptr when the range is outside the buffer.
  const uint8_t* GuestPhysicalForRead(uint32_t start, uint32_t length) const;
  // Advances with every invalidation (CPU writes, published host writes,
  // resets); a cache that skips requests re-checks residency when it moves.
  uint64_t invalidation_epoch() const {
    return invalidation_epoch_.load(std::memory_order_acquire);
  }

  // Marks the range and, if not exact_range, potentially its surroundings
  // (to up to the first GPU-written page, as an access violation exception
  // count optimization) as modified by the CPU, also invalidating GPU-written
  // pages directly in the range.
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t physical_address_start,
                                                           uint32_t length, bool exact_range);

  // Host writes of CPU-side data made inside the GPU plugin (XMA output,
  // command-processor register, fence and query writes), which the host's
  // guest-store tracker never sees. Marking is lock-free and allowed from any
  // thread after the write; the pages are published (invalidated in every GPU
  // cache, through the publisher) on the command processor thread before a
  // request touches them and by PublishHostWrites, once per frame.
  using HostWritePublisher = void (*)(void* context, uint32_t start, uint32_t length);
  void SetHostWritePublisher(HostWritePublisher publisher, void* context) {
    host_write_publisher_ = publisher;
    host_write_publisher_context_ = context;
  }
  void MarkHostWrite(uint32_t start, uint32_t length);
  void PublishHostWrites();

  // Bounded, in-memory ordering evidence for one dynamically selected texture
  // range. Guest write callbacks may be extremely hot, so events are recorded
  // without synchronous output and dumped later by the draw path.
  enum class TextureLifecycleDiagnosticEventType : uint32_t {
    kRangeRegistered,
    kPrepare,
    kCommitBeforeLoad,
    kSharedUploadCopyBegin,
    kSharedUploadCopyEnd,
    kD3D12LoadBegin,
    kHostTextureCreated,
    kD3D12LoadRecorded,
    kWatchInstalled,
    kPhysicalWriteNotify,
    kInvalidationExecute,
    kWatchInvalidated,
    kDescriptorCreated,
    kDrawBinding,
    kShaderViewSelected,
    kCpuUntileExpected,
    kGpuUntileReadbackQueued,
    kGpuUntileReadbackReady,
    kHostTextureReadbackReady,
  };
  void BeginTextureLifecycleDiagnostic(uint32_t physical_address_start, uint32_t length);
  bool TextureLifecycleDiagnosticOverlaps(uint32_t physical_address_start,
                                          uint32_t length) const;
  bool CopyTextureLifecycleDiagnosticSource(uint32_t physical_address_start, uint32_t length,
                                            void* output) const;
  void RecordTextureLifecycleDiagnosticEvent(TextureLifecycleDiagnosticEventType type,
                                             uint32_t physical_address_start, uint32_t length,
                                             uint64_t value0 = 0, uint64_t value1 = 0,
                                             bool hash_tracked_source = false);
  void DumpTextureLifecycleDiagnosticEvents();

  // Marks the range as containing GPU-generated data (such as resolves),
  // triggering modification callbacks, making it valid (so pages are not
  // copied from the main memory until they're modified by the CPU) and
  // protecting it. Before writing anything from the GPU side, RequestRange must
  // be called, to make sure, if the GPU writes don't overwrite *everything* in
  // the pages they touch, the CPU data is properly loaded to the unmodified
  // regions in those pages.
  // A producer may have already handled its OWN global watch with more precise
  // write geometry under the global critical region. Skip only that callback
  // for this call; all other and per-resource watches still run. Nested/CPU
  // writes do not inherit the exception. Default preserves all notifications.
  void RangeWrittenByGpu(uint32_t start, uint32_t length,
                         GlobalWatchHandle already_handled_watch = nullptr);

 protected:
  SharedMemory(memory::Memory& memory);
  // Call in implementation-specific initialization.
  void InitializeCommon();
  void InitializeSparseHostGpuMemory(uint32_t granularity_log2);
  // Call last in implementation-specific shutdown, also callable from the
  // destructor.
  void ShutdownCommon();

  // Sparse allocations are 4 MB, so not too many of them are allocated, but
  // also not to waste too much memory for padding (with 16 MB there's too
  // much).
  static constexpr uint32_t kHostGpuMemoryOptimalSparseAllocationLog2 = 22;
  static_assert(kHostGpuMemoryOptimalSparseAllocationLog2 <= kBufferSizeLog2);

  memory::Memory& memory() const { return memory_; }

  uint32_t page_size_log2() const { return page_size_log2_; }

  uint32_t host_gpu_memory_sparse_granularity_log2() const {
    return host_gpu_memory_sparse_granularity_log2_;
  }

  // Allocations in the host buffer are aligned the same way as in the guest
  // physical memory (for instance, if an allocation is 64 KB, it can represent
  // 0-64 KB, 64-128 KB, 128-192 KB in the guest memory, and so on, but not
  // something like 16-80 KB. This is assumed by the rules for texture data
  // access in the texture cache.
  virtual bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                uint32_t length_allocations);

  // Mark the memory range as updated and protect it.
  void MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu);

  // Uploads a range of host pages - only called if host GPU sparse memory
  // allocation succeeded if needed. While uploading, MakeRangeValid must be
  // called for each successfully uploaded range as early as possible, before
  // the memcpy, to make sure invalidation that happened during the CPU -> GPU
  // memcpy isn't missed (upload_page_ranges is in pages because of this -
  // MakeRangeValid has page granularity). upload_page_ranges are sorted in
  // ascending address order, so front and back can be used to determine the
  // overall bounds of pages to be uploaded.
  virtual bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) = 0;

#if REX_GPU_DIAGNOSTICS
  bool coherency_audit_enabled() const { return coherency_audit_enabled_; }
  // Around one uploaded chunk: before MakeRangeValid, remember which pages were
  // still valid apart from the frame-end reset; after the copy, hash them.
  void CoherencyAuditBeforeUpload(uint32_t page_first, uint32_t page_count);
  void CoherencyAuditAfterUpload(uint32_t page_first, uint32_t page_count, const uint8_t* data);
#endif

  const std::vector<std::pair<uint32_t, uint32_t>>& trace_download_ranges() {
    return trace_download_ranges_;
  }
  uint32_t trace_download_page_count() const { return trace_download_page_count_; }
  // Fills trace_download_ranges() and trace_download_page_count() with
  // GPU-written ranges that need to be downloaded, and also invalidates
  // non-GPU-written ranges so only the needed data - not the all the collected
  // data - will be written in the trace. trace_download_page_count() will be 0
  // if nothing to download.
  void PrepareForTraceDownload();
  // Release memory used for trace download ranges, to be called after
  // downloading or in cases when download is dropped.
  void ReleaseTraceDownloadRanges();

 private:
  memory::Memory& memory_;

  // Log2 of invalidation granularity (the system page size, but the dependency
  // on it is not hard - the access callback takes a range as an argument, and
  // touched pages of the buffer of this size will be invalidated).
  uint32_t page_size_log2_;

  bool EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length);
  uint32_t host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> host_gpu_memory_sparse_allocated_;
  uint32_t host_gpu_memory_sparse_allocations_ = 0;
  uint32_t host_gpu_memory_sparse_used_bytes_ = 0;

  void* memory_invalidation_callback_handle_ = nullptr;
  void* memory_data_provider_handle_ = nullptr;

  // Ranges that need to be uploaded, generated by GetRangesToUpload (a
  // persistently allocated vector).
  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;
  // Sorted/merged request ranges, reused by RequestRanges (command processor
  // thread) so per-draw buffer requests don't allocate.
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges_scratch_;

  // Pending host writes (MarkHostWrite): one bit per 4 KB page, plus one
  // summary bit per page word. Set with read-modify-writes only (never
  // skipped), so a publication that consumes a bit also sees the write.
  static constexpr uint32_t kHostWritePageCount = kBufferSize >> 12;
  static constexpr uint32_t kHostWriteWordCount = kHostWritePageCount / 64;
  std::array<std::atomic<uint64_t>, kHostWriteWordCount> host_write_pages_{};
  std::array<std::atomic<uint64_t>, kHostWriteWordCount / 64> host_write_summary_{};
  HostWritePublisher host_write_publisher_ = nullptr;
  void* host_write_publisher_context_ = nullptr;
  bool HostWritesPendingIn(uint32_t start, uint32_t length) const;

  // GPU-written memory downloading for traces. <Start address, length>.
  std::vector<std::pair<uint32_t, uint32_t>> trace_download_ranges_;
  uint32_t trace_download_page_count_ = 0;

  // Mutex between the guest memory subsystem and the command processor, to be
  // locked when checking or updating validity of pages/ranges and when firing
  // watches.
  rex::thread::global_critical_region global_critical_region_;

  // ***************************************************************************
  // Things below should be fully protected by global_critical_region.
  // ***************************************************************************

  // Double-buffered valid-page flags for lockless checks in RequestRanges.
  std::vector<uint64_t> valid_buffer_a_;
  std::vector<uint64_t> valid_buffer_b_;
  std::atomic<uint64_t*> active_valid_flags_{nullptr};
  std::atomic<uint64_t*> staging_valid_flags_{nullptr};
  // Subset of valid pages containing data written by the GPU.
  std::vector<uint64_t> system_page_flags_valid_and_gpu_written_;
  // Dirty state tracking for frame-end page-state refresh.
  std::atomic<bool> gpu_written_data_dirty_{false};
  std::atomic<uint32_t> dirty_blocks_{0};
  uint32_t num_system_page_flags_ = 0;
  // See invalidation_epoch(); advanced (release) after the valid bits change.
  std::atomic<uint64_t> invalidation_epoch_{0};

#if REX_GPU_DIAGNOSTICS
  bool coherency_audit_enabled_ = false;
  // Pages that would still be valid without the frame-end reset: set by
  // MakeRangeValid, cleared only by invalidation (global critical region).
  std::vector<uint64_t> audit_tracked_valid_;
  // Per page: hash of the last CPU upload (0 = none, or GPU-written since).
  std::vector<uint64_t> audit_page_hash_;
  // Per page: sequence number of the last invalidation that covered it.
  std::vector<uint64_t> audit_page_invalidation_;
  uint64_t audit_invalidation_sequence_ = 0;
  // The chunk being uploaded (command processor thread).
  std::vector<uint8_t> audit_chunk_tracked_;
  std::vector<uint64_t> audit_chunk_hash_;
  uint64_t audit_chunk_sequence_ = 0;
  struct AuditMismatch {
    uint32_t page;
    uint64_t sequence;
    uint64_t frame;
  };
  // Changed pages whose invalidation may still arrive (a write published at
  // the next submission boundary); classified at frame end.
  std::vector<AuditMismatch> audit_pending_mismatches_;
  struct AuditCounts {
    uint64_t frames = 0;
    uint64_t upload_chunks = 0;
    // Uploads needed anyway: first use, or invalidated by a CPU write.
    uint64_t tracked_pages = 0;
    // Uploads caused only by the frame-end reset.
    uint64_t clear_only_pages = 0;
    // Invalidated while the chunk was being uploaded.
    uint64_t racing_pages = 0;
    uint64_t mismatches = 0;
    uint64_t late_notified = 0;
    uint64_t unnotified = 0;
    // MemoryInvalidationCallback calls (guest drains and plugin host writes).
    uint64_t invalidations = 0;
    // Rolling sweep: valid CPU-uploaded pages re-hashed against guest memory,
    // and those found changed (also counted in mismatches).
    uint64_t sweep_pages = 0;
    uint64_t sweep_mismatches = 0;
    // Invalidated more than kGraceFrames after the change was seen (tracked
    // writes the guest submitted late), and the longest such delay.
    uint64_t late_slow = 0;
    uint64_t late_slow_max_frames = 0;
    // The GPU requested (or found resident) a page while its change was still
    // unexplained, attributed when the change is classified: benign when its
    // invalidation came within kGraceFrames (commands issued before the
    // write's submission), suspect otherwise (late_slow or unnotified).
    uint64_t stale_uses = 0;
    uint64_t stale_uses_suspect = 0;
  };
  // Pages with an unexplained change (pending mismatch), for stale uses, and
  // the uses counted while the change is pending.
  std::vector<uint8_t> audit_page_mismatch_;
  std::vector<uint32_t> audit_page_pending_uses_;
  static constexpr uint32_t kAuditStaleUseAddressCapacity = 8;
  uint32_t audit_stale_use_addresses_[kAuditStaleUseAddressCapacity] = {};
  uint32_t audit_stale_use_address_count_ = 0;
  void CoherencyAuditUsed(uint32_t start, uint32_t length);
  void CoherencyAuditPushMismatch(uint32_t page);

 public:
  // Measurement builds: a per-draw cache skipped requesting this range because
  // it was resident (counts stale uses like RequestRanges does).
  void CoherencyAuditSkippedRequest(uint32_t start, uint32_t length) {
    if (coherency_audit_enabled_) {
      CoherencyAuditUsed(start, length);
    }
  }

 private:
  // Rolling sweep state (command processor thread): next page word to scan.
  uint32_t audit_sweep_cursor_ = 0;
  std::vector<uint32_t> audit_sweep_pages_;
  std::vector<uint64_t> audit_sweep_hashes_;
  void CoherencyAuditSweep();
  AuditCounts audit_window_;
  AuditCounts audit_total_;
  uint64_t audit_frame_ = 0;
  static constexpr uint32_t kAuditAddressCapacity = 16;
  uint32_t audit_unnotified_addresses_[kAuditAddressCapacity] = {};
  uint32_t audit_unnotified_address_count_ = 0;
  // Last uploaded bytes of pages that changed without an invalidation, to log
  // what changed (REX_UPLOAD_COHERENCY_DIFF) the next time they change.
  static constexpr size_t kAuditPageCopyCapacity = 8;
  struct AuditPageCopy {
    uint32_t page;
    uint32_t diffs;
    std::vector<uint8_t> bytes;  // Empty until the next upload of the page.
  };
  std::vector<AuditPageCopy> audit_page_copies_;
  uint32_t audit_diff_logs_ = 0;
  void CoherencyAuditInvalidated(uint32_t page_first, uint32_t page_last);
  void CoherencyAuditLogDiff(uint32_t page, const uint8_t* old_data, const uint8_t* new_data);
#endif

  static constexpr uint32_t kTextureLifecycleDiagnosticEventCapacity = 2048;
  struct TextureLifecycleDiagnosticEvent {
    std::atomic<uint64_t> ready_sequence{0};
    uint64_t host_microseconds = 0;
    TextureLifecycleDiagnosticEventType type =
        TextureLifecycleDiagnosticEventType::kRangeRegistered;
    uint32_t address = 0;
    uint32_t length = 0;
    uint32_t hash_first = 0;
    uint32_t hash_second = 0;
    uint64_t value0 = 0;
    uint64_t value1 = 0;
  };
  std::atomic<uint32_t> texture_lifecycle_diagnostic_base_plus_one_{0};
  std::atomic<uint32_t> texture_lifecycle_diagnostic_length_{0};
  std::atomic<uint64_t> texture_lifecycle_diagnostic_event_count_{0};
  std::atomic<uint64_t> texture_lifecycle_diagnostic_dumped_count_{0};
  std::atomic<uint32_t> texture_lifecycle_diagnostic_draw_binding_count_{0};
  std::atomic<uint32_t> texture_lifecycle_diagnostic_shader_view_count_{0};
  std::array<TextureLifecycleDiagnosticEvent, kTextureLifecycleDiagnosticEventCapacity>
      texture_lifecycle_diagnostic_events_{};

  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

  struct GlobalWatch {
    GlobalWatchCallback callback;
    void* callback_context;
  };
  std::vector<GlobalWatch*> global_watches_;
  struct WatchNode;
  // Watched range placed by other GPU subsystems.
  struct WatchRange {
    union {
      struct {
        WatchCallback callback;
        void* callback_context;
        void* callback_data;
        uint64_t callback_argument;
        WatchNode* node_first;
        uint32_t page_first;
        uint32_t page_last;
      };
      WatchRange* next_free;
    };
  };
  // Node for faster checking of watches when pages have been written to - all
  // 512 MB are split into smaller equally sized buckets, and then ranges are
  // linearly checked.
  struct WatchNode {
    union {
      struct {
        WatchRange* range;
        // Link to another node of this watched range in the next bucket.
        WatchNode* range_node_next;
        // Links to nodes belonging to other watched ranges in the bucket.
        WatchNode* bucket_node_previous;
        WatchNode* bucket_node_next;
      };
      WatchNode* next_free;
    };
  };
  static constexpr uint32_t kWatchBucketSizeLog2 = 22;
  static constexpr uint32_t kWatchBucketCount = 1 << (kBufferSizeLog2 - kWatchBucketSizeLog2);
  WatchNode* watch_buckets_[kWatchBucketCount] = {};
  // Allocation from pools - taking new WatchRanges and WatchNodes from the free
  // list, and if there are none, creating a pool if the current one is fully
  // used, and linearly allocating from the current pool.
  static constexpr uint32_t kWatchRangePoolSize = 8192;
  static constexpr uint32_t kWatchNodePoolSize = 8192;
  std::vector<WatchRange*> watch_range_pools_;
  std::vector<WatchNode*> watch_node_pools_;
  uint32_t watch_range_current_pool_allocated_ = 0;
  uint32_t watch_node_current_pool_allocated_ = 0;
  WatchRange* watch_range_first_free_ = nullptr;
  WatchNode* watch_node_first_free_ = nullptr;
  // Triggers the watches (global and per-range), removing triggered range
  // watches.
  void FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu,
                   GlobalWatchHandle already_handled_watch = nullptr);
  // Unlinks and frees the range and its nodes. Call this in the global critical
  // region.
  void UnlinkWatchRange(WatchRange* range);
};

}  // namespace rex::graphics
