#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace rex::graphics::d3d12 {

// A diagnostic copy, never a submission boundary. Keep this object alive until
// its recorded submission fence completes. Source state and command-list-local
// graphics bindings are unchanged after Enqueue. In particular, there is no
// CPU wait, command-list reset, pipeline change or duplicate draw here.
struct DiagnosticColorReadback {
  Microsoft::WRL::ComPtr<ID3D12Resource> source;
  Microsoft::WRL::ComPtr<ID3D12Resource> resolved;
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  D3D12_RESOURCE_DESC source_desc = {};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 row_bytes = 0;
  UINT64 buffer_bytes = 0;

  bool Create(ID3D12Device* device, ID3D12Resource* resource, uint64_t limit) {
    if (!device || !resource || source || !limit) return false;
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        !desc.Width || !desc.Height || desc.Width > limit / 8 / desc.Height ||
        !desc.SampleDesc.Count || desc.SampleDesc.Count > 8 ||
        (desc.SampleDesc.Count & (desc.SampleDesc.Count - 1))) return false;

    auto copy_desc = desc;
    if (desc.SampleDesc.Count > 1) {
      copy_desc.Alignment = 0;
      copy_desc.SampleDesc = {1, 0};
      copy_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES heap = {};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      heap.CreationNodeMask = heap.VisibleNodeMask = 1;
      if (FAILED(device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &copy_desc,
              D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
              IID_PPV_ARGS(&resolved)))) return false;
    }
    device->GetCopyableFootprints(&copy_desc, 0, 1, 0, &footprint,
                                  &rows, &row_bytes, &buffer_bytes);
    if (rows != desc.Height || row_bytes != desc.Width * 8 ||
        buffer_bytes > limit || !buffer_bytes) return false;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = buffer_bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&buffer)))) return false;
    source = resource;
    source_desc = desc;
    return true;
  }

  template <typename Commands>
  void Enqueue(Commands& commands, D3D12_RESOURCE_STATES original_state) const {
    const auto transition = [&](ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES before,
                                D3D12_RESOURCE_STATES after) {
      if (before == after) return;
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = resource;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = before;
      barrier.Transition.StateAfter = after;
      commands.D3DResourceBarrier(1, &barrier);
    };
    const auto copy_state = resolved ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                    : D3D12_RESOURCE_STATE_COPY_SOURCE;
    transition(source.Get(), original_state, copy_state);
    if (resolved) {
      commands.D3DResolveSubresource(resolved.Get(), 0, source.Get(), 0,
                                     source_desc.Format);
      transition(resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = resolved ? resolved.Get() : source.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = buffer.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    commands.D3DCopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(source.Get(), copy_state, original_state);
  }
};

}  // namespace rex::graphics::d3d12
