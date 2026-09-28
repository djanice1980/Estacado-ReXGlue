#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace rex::graphics::d3d12 {

// A bounded GPU sample extraction, not a resolve or submission boundary. The
// caller sets pipeline through SetExternalPipeline before Enqueue, before the
// next guest pipeline binding, and retains this object until its fence completes.
struct DiagnosticColorSamplesReadback {
  Microsoft::WRL::ComPtr<ID3D12Resource> source, output, readback;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  D3D12_RESOURCE_DESC source_desc = {};
  uint32_t left = 0, top = 0, width = 0, height = 0, samples = 0;
  uint64_t bytes = 0;

  bool Create(ID3D12Device* device, ID3D12Resource* resource,
              uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t limit,
              D3D12_SHADER_BYTECODE single_shader, D3D12_SHADER_BYTECODE msaa_shader) {
    if (!device || !resource || source || !w || !h || !limit) return false;
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        (desc.SampleDesc.Count != 1 && desc.SampleDesc.Count != 2 && desc.SampleDesc.Count != 4) ||
        x > desc.Width || w > desc.Width - x || y > desc.Height || h > desc.Height - y ||
        uint64_t(w) > limit / 8 / h / desc.SampleDesc.Count) return false;
    const uint64_t size = uint64_t(w) * h * desc.SampleDesc.Count * 8;
    // The shader uses 32-bit byte offsets and D3D12 dispatch dimensions.
    if (size > UINT32_MAX || w > 65535 * 8u || h > 65535 * 8u) return false;

    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size; buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)))) return false;
    heap.Type = D3D12_HEAP_TYPE_READBACK; buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return false;
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[3] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 5;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 3; root_desc.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, nullptr)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = {};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = desc.SampleDesc.Count == 1 ? single_shader : msaa_shader;
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)))) return false;
    source = resource; source_desc = desc;
    left = x; top = y; width = w; height = h; samples = desc.SampleDesc.Count; bytes = size;
    return true;
  }

  void WriteSRV(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = source_desc.Format;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = samples == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
    if (samples == 1) view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(source.Get(), &view, handle);
  }

  template <typename Commands>
  void Enqueue(Commands& commands, D3D12_RESOURCE_STATES previous,
                D3D12_GPU_DESCRIPTOR_HANDLE srv) const {
    const auto transition = [&](ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
      if (before == after) return;
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
      commands.D3DResourceBarrier(1, &barrier);
    };
    transition(source.Get(), previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commands.D3DSetComputeRootSignature(root.Get());
    const uint32_t constants[] = {left, top, width, height, samples};
    commands.D3DSetComputeRoot32BitConstants(0, 5, constants, 0);
    commands.D3DSetComputeRootDescriptorTable(1, srv);
    commands.D3DSetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    commands.D3DDispatch((width + 7) / 8, (height + 7) / 8, samples);
    transition(source.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, previous);
    transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.D3DCopyBufferRegion(readback.Get(), 0, output.Get(), 0, bytes);
  }
};

}  // namespace rex::graphics::d3d12
