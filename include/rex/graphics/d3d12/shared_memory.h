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

#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/graphics/embedded_camera_geometry_capture_policy.h>
#include <rex/graphics/embedded_scene_resolve_capture_policy.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/d3d12/upload_copy_worker.h>
#include <rex/memory.h>
#include <rex/ui/d3d12/d3d12_api.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

class D3D12SharedMemory : public SharedMemory {
 public:
  D3D12SharedMemory(D3D12CommandProcessor& command_processor, memory::Memory& memory,
                    TraceWriter& trace_writer);
  ~D3D12SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  ID3D12Resource* GetBuffer() const { return buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const { return buffer_gpu_address_; }

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  // Copy stage (d3d12_async_upload_copies, upload_copy_worker.h). Command
  // processor thread only: the ticket of the last queued guest-memory copy
  // (0 when none was ever queued).
  uint64_t QueuedUploadCopies() const {
    return upload_copy_worker_ ? upload_copy_worker_->queued() : 0;
  }
  uint64_t CompletedUploadCopies() const {
    return upload_copy_worker_ ? upload_copy_worker_->completed() : 0;
  }
  // Any thread: returns once every copy up to ticket has been written.
  void WaitForUploadCopies(uint64_t ticket) const {
    if (ticket && upload_copy_worker_) upload_copy_worker_->Wait(ticket);
  }

  // Bounded exact-draw observer. No CPU substitute, submission split or wait.
  // Source is the already-resident shared GPU buffer used by this draw.
  bool QueueGeometryReadback(uint32_t address, uint64_t requested_bytes,
                             const char* path, uint64_t draw);
  bool QueueTextureSourceReadback(uint32_t address, uint64_t requested_bytes,
                                  const char* path, uint64_t draw);
  bool QueueCameraGeometryReadback(uint32_t address, uint64_t requested_bytes,
                                   const char* path, uint64_t draw);
  bool QueueSceneResolveReadback(uint32_t address, uint64_t requested_bytes,
                                 const char* path, uint64_t draw);

  // RequestRange may transition the buffer to copy destination - call it before
  // UseForReading or UseForWriting.

  // Makes the buffer usable for vertices, indices and texture untiling.
  void UseForReading() {
    // Vertex fetch is also allowed in pixel shaders.
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_INDEX_BUFFER |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  // Makes the buffer usable for texture tiling after a resolve.
  void UseForWriting() {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  // Makes the buffer usable as a source for copy commands.
  void UseAsCopySource() { CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE); }
  // Must be called when doing draws/dispatches modifying data within the shared
  // memory buffer as a UAV, to make sure that when UseForWriting is called the
  // next time, a UAV barrier will be done, and subsequent overlapping UAV
  // writes and reads are ordered.
  void MarkUAVWritesCommitNeeded() {
    if (buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      buffer_uav_writes_commit_needed_ = true;
    }
  }

  void WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  // Due to the D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP limitation, the
  // smallest supported formats are 32-bit.
  void WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                  uint32_t element_size_bytes_pow2);
  void WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                  uint32_t element_size_bytes_pow2);

  // Returns true if any downloads were submitted to the command processor.
  bool InitializeTraceSubmitDownloads();
  void InitializeTraceCompleteDownloads();

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  bool QueueBoundedReadback(uint32_t address, uint32_t bytes, const char* path,
                           uint64_t draw, bool texture_source, bool camera_geometry = false,
                           bool scene_resolve = false);
  D3D12CommandProcessor& command_processor_;
  TraceWriter& trace_writer_;

  // The 512 MB tiled buffer.
  ID3D12Resource* buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS buffer_gpu_address_ = 0;
  std::vector<ID3D12Heap*> buffer_tiled_heaps_;
  D3D12_RESOURCE_STATES buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  bool buffer_uav_writes_commit_needed_ = false;
  void CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state);

  // Non-shader-visible buffer descriptor heap for faster binding (via copying
  // rather than creation).
  enum class BufferDescriptorIndex : uint32_t {
    kRawSRV,
    kR32UintSRV,
    kR32G32UintSRV,
    kR32G32B32A32UintSRV,
    kRawUAV,
    kR32UintUAV,
    kR32G32UintUAV,
    kR32G32B32A32UintUAV,

    kCount,
  };
  ID3D12DescriptorHeap* buffer_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE buffer_descriptor_heap_start_;

  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool> upload_buffer_pool_;
  // Created on first use; destroyed (after its last copy) before the pool.
  std::unique_ptr<UploadCopyWorker> upload_copy_worker_;

  // Created temporarily, only for downloading.
  ID3D12Resource* trace_download_buffer_ = nullptr;
  void ResetTraceDownload();

  struct GeometryReadback {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    uint64_t submission = 0;
    uint64_t draw = 0;
    uint32_t address = 0;
    uint32_t bytes = 0;
    std::string path;
    bool texture_source = false;
    bool camera_geometry = false;
    bool scene_resolve = false;
  };
  uint32_t geometry_readback_count_ = 0;
  uint32_t texture_source_readback_count_ = 0;
  embedded_camera_geometry_capture_policy::RangeBudget camera_geometry_budget_;
  embedded_scene_resolve_capture_policy::RangeBudget scene_resolve_budget_;
  std::vector<GeometryReadback> geometry_readbacks_;
};

}  // namespace rex::graphics::d3d12
