/**
 * @file        ui/d3d12/d3d12_present_policy.h
 * @brief       Pure D3D12 host presentation policy selection
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <dxgi1_6.h>

namespace rex::ui::d3d12 {

enum class HostPresentMode : uint8_t {
  kVsync,
  kImmediate,
  kVariableRefreshRate,
};

struct HostPresentParameters {
  UINT sync_interval;
  UINT flags;
  bool variable_refresh_rate_active;
};

// IDXGISwapChain2::SetMaximumFrameLatency is valid only for swap chains
// created with FRAME_LATENCY_WAITABLE_OBJECT. The flag is immutable across
// ResizeBuffers, so creation and resize must use this same policy. The
// waitable-object contract also keeps latency ownership per swap chain rather
// than mutating a device-global queue limit.
inline UINT ResolveHostSwapChainFlags(bool allow_tearing) {
  UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (allow_tearing) {
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }
  return flags;
}

inline std::optional<HostPresentMode> ParseHostPresentMode(
    std::string_view value) {
  if (value == "vsync") {
    return HostPresentMode::kVsync;
  }
  if (value == "immediate") {
    return HostPresentMode::kImmediate;
  }
  if (value == "vrr") {
    return HostPresentMode::kVariableRefreshRate;
  }
  return std::nullopt;
}

// Guest VBlank generation is deliberately outside this policy. This function
// only decides how an already completed guest frame is handed to DXGI.
inline HostPresentParameters ResolveHostPresentParameters(
    HostPresentMode mode, bool swap_chain_allows_tearing) {
  switch (mode) {
    case HostPresentMode::kImmediate:
      return {
          0,
          swap_chain_allows_tearing ? UINT(DXGI_PRESENT_ALLOW_TEARING) : 0,
          false,
      };
    case HostPresentMode::kVariableRefreshRate:
      if (swap_chain_allows_tearing) {
        return {0, UINT(DXGI_PRESENT_ALLOW_TEARING), true};
      }
      // VRR requires ALLOW_TEARING on a flip-model swap chain. Prefer the
      // tear-free synchronized fallback rather than silently becoming an
      // unsynchronized non-VRR mode.
      return {1, 0, false};
    case HostPresentMode::kVsync:
    default:
      return {1, 0, false};
  }
}

}  // namespace rex::ui::d3d12
