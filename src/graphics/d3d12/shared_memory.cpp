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

#include <cstring>
#include <cstdio>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/graphics/embedded_geometry_readback_policy.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_tiled_shared_memory, true, "GPU/D3D12",
                    "Use tiled shared memory on D3D12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Command-processor copy stage: guest-memory copies into upload pages run on
// a worker thread; submissions and guest-visible writes wait for them
// (upload_copy_worker.h). Read per upload, so it can be switched live.
REXCVAR_DEFINE_BOOL(d3d12_async_upload_copies, false, "GPU/D3D12",
                    "Copy guest memory into upload pages on a worker thread");

namespace rex::graphics::d3d12 {

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     memory::Memory& memory, TraceWriter& trace_writer)
    : SharedMemory(memory), command_processor_(command_processor), trace_writer_(trace_writer) {}

D3D12SharedMemory::~D3D12SharedMemory() {
  Shutdown(true);
}

bool D3D12SharedMemory::Initialize() {
  InitializeCommon();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kBufferSize,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (REXCVAR_GET(d3d12_tiled_shared_memory) &&
      provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
      !provider.GetGraphicsAnalysis()) {
    if (FAILED(device->CreateReservedResource(&buffer_desc, buffer_state_, nullptr,
                                              IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB tiled buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
    InitializeSparseHostGpuMemory(
        std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
  } else {
    REXGPU_INFO(
        "Direct3D 12 tiled resources are not used for shared memory "
        "emulation - video memory usage may increase significantly "
        "because a full {} MB buffer will be created",
        kBufferSize >> 20);
    if (provider.GetGraphicsAnalysis()) {
      // As of October 8th, 2018, PIX doesn't support tiled buffers.
      // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
      REXGPU_INFO(
          "This is caused by PIX being attached, which doesn't support tiled "
          "resources yet.");
    }
    if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                               provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                               buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
  }
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors = uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    REXGPU_ERROR("Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ = buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintSRV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintUAV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);

  upload_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, rex::align(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           size_t(1) << page_size_log2()));

  return true;
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  ResetTraceDownload();
  geometry_readbacks_.clear();
  geometry_readback_count_ = 0;
  texture_source_readback_count_ = 0;
  camera_geometry_budget_ = {};
  scene_resolve_budget_ = {};

  // Finishes its queued copies into upload pages before the pool goes away.
  upload_copy_worker_.reset();
  upload_buffer_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // First free the buffer to detach it from the heaps.
  ui::d3d12::util::ReleaseAndNull(buffer_);

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  for (auto it = geometry_readbacks_.begin(); it != geometry_readbacks_.end();) {
    if (!embedded_geometry_readback_policy::MayMap(it->submission, completed)) {
      ++it;
      continue;
    }
    void* data = nullptr;
    const D3D12_RANGE range{0, it->bytes};
    size_t written = 0;
    uint32_t content_hash = 0;
    if (SUCCEEDED(it->buffer->Map(0, &range, &data))) {
      content_hash = embedded_geometry_readback_policy::ContentHash(
          static_cast<const uint8_t*>(data), it->bytes);
      if (FILE* file = std::fopen(it->path.c_str(), "wb")) {
        written = std::fwrite(data, 1, it->bytes, file);
        if (std::fclose(file) != 0) written = 0;
      }
      const D3D12_RANGE no_writes{0, 0};
      it->buffer->Unmap(0, &no_writes);
    }
    std::fprintf(stderr,
        "%s result=%u draw=%llu address=0x%08X "
        "bytes=%u written=%llu submission=%llu completed=%llu path=%s hash=%08X\n",
        it->scene_resolve ? "REX_SCENE_RESOLVE_READBACK" :
          it->camera_geometry ? "REX_EMBEDDED_CAMERA_GEOMETRY_READBACK" :
            (it->texture_source ? "REX_EMBEDDED_TEXTURE_SOURCE_READBACK" : "REX_EMBEDDED_GEOMETRY_READBACK"),
        written == it->bytes ? 1u : 0u, static_cast<unsigned long long>(it->draw),
        it->address, it->bytes, static_cast<unsigned long long>(written),
        static_cast<unsigned long long>(it->submission),
        static_cast<unsigned long long>(completed), it->path.c_str(), content_hash);
    it = geometry_readbacks_.erase(it);
  }
}

bool D3D12SharedMemory::QueueGeometryReadback(uint32_t address, uint64_t requested_bytes,
                                            const char* path, uint64_t draw) {
  const uint32_t bytes = embedded_geometry_readback_policy::CaptureBytes(
      address, requested_bytes, geometry_readback_count_);
  return QueueBoundedReadback(address, bytes, path, draw, false);
}

bool D3D12SharedMemory::QueueTextureSourceReadback(uint32_t address, uint64_t requested_bytes,
                                                const char* path, uint64_t draw) {
  const uint32_t bytes = embedded_geometry_readback_policy::TextureBytes(
      address, requested_bytes, texture_source_readback_count_);
  return QueueBoundedReadback(address, bytes, path, draw, true);
}

bool D3D12SharedMemory::QueueCameraGeometryReadback(uint32_t address, uint64_t requested_bytes,
                                                 const char* path, uint64_t draw) {
  const uint32_t bytes = camera_geometry_budget_.Reserve(address, requested_bytes);
  return QueueBoundedReadback(address, bytes, path, draw, false, true);
}

bool D3D12SharedMemory::QueueBoundedReadback(uint32_t address, uint32_t bytes,
                                         const char* path, uint64_t draw, bool texture_source,
                                         bool camera_geometry, bool scene_resolve) {
  // The exact-draw caller has an open submission; GetCurrentSubmission must
  // also be nonzero so an unsubmitted copy can never pass the completion gate.
  if (!bytes || !path || !*path || !command_processor_.GetCurrentSubmission()) return false;
  const auto& provider = command_processor_.GetD3D12Provider();
  GeometryReadback record;
  D3D12_RESOURCE_DESC desc;
  ui::d3d12::util::FillBufferResourceDesc(desc, bytes, D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
      &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
      &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&record.buffer)))) return false;
  record.submission = command_processor_.GetCurrentSubmission();
  record.draw = draw;
  record.address = address;
  record.bytes = bytes;
  record.path = path;
  record.texture_source = texture_source;
  record.camera_geometry = camera_geometry;
  record.scene_resolve = scene_resolve;
  const D3D12_RESOURCE_STATES original_state = buffer_state_;
  UseAsCopySource();
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      record.buffer.Get(), 0, buffer_, address, bytes);
  CommitUAVWritesAndTransitionBuffer(original_state);
  command_processor_.SubmitBarriers();
  // No command-list close/execute/reset: previously bound graphics state survives.
  if (!scene_resolve) {
    if (texture_source) ++texture_source_readback_count_;
    else if (!camera_geometry) ++geometry_readback_count_;
  }
  geometry_readbacks_.push_back(std::move(record));
  return true;
}

bool D3D12SharedMemory::QueueSceneResolveReadback(uint32_t address, uint64_t requested_bytes,
                                                const char* path, uint64_t draw) {
  const uint32_t bytes = scene_resolve_budget_.Reserve(address, requested_bytes);
  return QueueBoundedReadback(address, bytes, path, draw, false, false, true);
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state) {
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();
  PrepareForTraceDownload();
  uint32_t download_page_count = trace_download_page_count();
  if (!download_page_count) {
    return false;
  }
  D3D12_RESOURCE_DESC download_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      download_buffer_desc, download_page_count << page_size_log2(), D3D12_RESOURCE_FLAG_NONE);
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             provider.GetHeapFlagCreateNotZeroed(),
                                             &download_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&trace_download_buffer_)))) {
    REXGPU_ERROR(
        "Shared memory: Failed to create a {} KB GPU-written memory download "
        "buffer for frame tracing",
        download_page_count << page_size_log2() >> 10);
    ResetTraceDownload();
    return false;
  }
  auto& command_list = command_processor_.GetDeferredCommandList();
  UseAsCopySource();
  command_processor_.SubmitBarriers();
  uint32_t download_buffer_offset = 0;
  for (const auto& download_range : trace_download_ranges()) {
    command_list.D3DCopyBufferRegion(trace_download_buffer_, download_buffer_offset, buffer_,
                                     download_range.first, download_range.second);
    download_buffer_offset += download_range.second;
  }
  return true;
}

void D3D12SharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_buffer_) {
    return;
  }
  void* download_mapping;
  if (SUCCEEDED(trace_download_buffer_->Map(0, nullptr, &download_mapping))) {
    uint32_t download_buffer_offset = 0;
    for (const auto& download_range : trace_download_ranges()) {
      trace_writer_.WriteMemoryRead(
          download_range.first, download_range.second,
          reinterpret_cast<const uint8_t*>(download_mapping) + download_buffer_offset);
    }
    D3D12_RANGE download_write_range = {};
    trace_download_buffer_->Unmap(0, &download_write_range);
  } else {
    REXGPU_ERROR(
        "Shared memory: Failed to map the GPU-written memory download buffer "
        "for frame tracing");
  }
  ResetTraceDownload();
}

void D3D12SharedMemory::ResetTraceDownload() {
  ui::d3d12::util::ReleaseAndNull(trace_download_buffer_);
  ReleaseTraceDownloadRanges();
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations << host_gpu_memory_sparse_granularity_log2();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    REXGPU_ERROR("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X = offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = FALSE;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  direct_queue->UpdateTileMappings(buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
                                   &range_flags, &heap_range_start_offset, &region_size.NumTiles,
                                   D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

bool D3D12SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  D3D12CommandProcessor::GpuTimingScope upload_timing(command_processor_,
                                                      GpuTimingCategory::kUpload);
  command_processor_.GpuTimingCount(GpuTimingCounter::kUploadBatches, 1);
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& command_list = command_processor_.GetDeferredCommandList();
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());
    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        return false;
      }
      const uint32_t upload_physical_start = upload_range_start << page_size_log2();
      // The texture lifecycle trace is a measurement-build diagnostic.
      if (kGpuDiagnostics && TextureLifecycleDiagnosticOverlaps(upload_physical_start,
                                                                uint32_t(upload_buffer_size))) {
        RecordTextureLifecycleDiagnosticEvent(
            TextureLifecycleDiagnosticEventType::kSharedUploadCopyBegin,
            upload_physical_start, uint32_t(upload_buffer_size),
            uint64_t(reinterpret_cast<uintptr_t>(upload_buffer)), upload_buffer_offset, true);
      }
#if REX_GPU_DIAGNOSTICS
      const uint32_t audit_page_count = uint32_t(upload_buffer_size >> page_size_log2());
      if (coherency_audit_enabled()) {
        CoherencyAuditBeforeUpload(upload_range_start, audit_page_count);
      }
#endif
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      const bool lifecycle_diagnostic =
          kGpuDiagnostics &&
          TextureLifecycleDiagnosticOverlaps(upload_physical_start, uint32_t(upload_buffer_size));
      const void* upload_source =
          memory().TranslatePhysical(upload_range_start << page_size_log2());
      if (REXCVAR_GET(d3d12_async_upload_copies) && !lifecycle_diagnostic) {
        // Copy stage: the GPU copy below is recorded now; the submission and
        // any guest-visible write wait for this CPU copy (ticket).
        if (!upload_copy_worker_) upload_copy_worker_ = std::make_unique<UploadCopyWorker>();
        upload_copy_worker_->Enqueue(upload_buffer_mapping, upload_source, upload_buffer_size);
      } else {
        std::memcpy(upload_buffer_mapping, upload_source, upload_buffer_size);
      }
#if REX_GPU_DIAGNOSTICS
      // Hash the guest source, not the write-combined upload page.
      if (coherency_audit_enabled()) {
        CoherencyAuditAfterUpload(upload_range_start, audit_page_count,
                                  static_cast<const uint8_t*>(upload_source));
      }
#endif
      if (lifecycle_diagnostic) {
        uint32_t upload_hash = 2166136261u;
        for (size_t byte_index = 0; byte_index < upload_buffer_size; ++byte_index) {
          upload_hash = (upload_hash ^ upload_buffer_mapping[byte_index]) * 16777619u;
        }
        RecordTextureLifecycleDiagnosticEvent(
            TextureLifecycleDiagnosticEventType::kSharedUploadCopyEnd,
            upload_physical_start, uint32_t(upload_buffer_size),
            uint64_t(reinterpret_cast<uintptr_t>(upload_buffer)), upload_hash, true);
      }
      command_list.D3DCopyBufferRegion(buffer_, upload_range_start << page_size_log2(),
                                       upload_buffer, UINT64(upload_buffer_offset),
                                       UINT64(upload_buffer_size));
      command_processor_.NoteSharedMemoryUpload(upload_buffer_size);
      command_processor_.GpuTimingCount(GpuTimingCounter::kUploadCopies, 1);
      command_processor_.GpuTimingCount(GpuTimingCounter::kUploadBytes, upload_buffer_size);
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
  }
  return true;
}

}  // namespace rex::graphics::d3d12
