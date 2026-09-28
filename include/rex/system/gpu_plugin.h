/**
 * @file        system/gpu_plugin.h
 * @brief       GPU emulation plugin ABI and host-side loader
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     One plugin exists today (rexgpu-xenos). The factory carries an
 *              ABI version so this can grow into a general plugin API later.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <rex/system/interfaces/graphics.h>
#include <rex/memory/host_write_scope.h>
#include <rex/graphics/pc_owned_camera_packet.h>

#if defined(_WIN32)
#define REX_GPU_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REX_GPU_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace rex::system {

// Bump on any change to GpuCreateInfo or to the IGraphicsSystem interface.
inline constexpr uint32_t kGpuPluginAbiVersion = 1;

inline constexpr const char* kGpuCreateSymbol = "rex_gpu_create";
inline constexpr const char* kGpuAbiVersionSymbol = "rex_gpu_abi_version";

// Static-recompilation embedding ABI. This exposes Xenos GPU services and the
// XMA hardware decoder over host-owned guest memory, but never a second
// Runtime, KernelState, or Xbox memory image.
inline constexpr uint32_t kEmbeddedGpuAbiVersion = 12;
inline constexpr const char* kEmbeddedGpuCreateSymbol = "rex_gpu_embedded_create";
inline constexpr const char* kEmbeddedGpuDestroySymbol = "rex_gpu_embedded_destroy";
inline constexpr const char* kEmbeddedGpuReadMmioSymbol = "rex_gpu_embedded_read_mmio";
inline constexpr const char* kEmbeddedGpuWriteMmioSymbol = "rex_gpu_embedded_write_mmio";
inline constexpr const char* kEmbeddedGpuSetInterruptSymbol = "rex_gpu_embedded_set_interrupt";
inline constexpr const char* kEmbeddedGpuInitializeRingSymbol =
    "rex_gpu_embedded_initialize_ring";
inline constexpr const char* kEmbeddedGpuEnableReadPointerWritebackSymbol =
    "rex_gpu_embedded_enable_read_pointer_writeback";
inline constexpr const char* kEmbeddedGpuNotifyPhysicalWriteSymbol =
    "rex_gpu_embedded_notify_physical_write";
// Optional diagnostics extension. Older hosts may omit it, but any layout or
// required-service change still bumps the embedded ABI above.
inline constexpr const char* kEmbeddedGpuNoteInputTransitionSymbol =
    "rex_gpu_embedded_note_input_transition";
inline constexpr const char* kEmbeddedGpuGetPcSettingsSymbol =
    "rex_gpu_embedded_get_pc_settings";
inline constexpr const char* kEmbeddedGpuPollKeyboardMouseSymbol =
    "rex_gpu_embedded_poll_keyboard_mouse";
inline constexpr const char* kEmbeddedXmaSetupSymbol = "rex_gpu_embedded_xma_setup";
inline constexpr const char* kEmbeddedXmaShutdownSymbol = "rex_gpu_embedded_xma_shutdown";
inline constexpr const char* kEmbeddedXmaAllocateSymbol = "rex_gpu_embedded_xma_allocate";
inline constexpr const char* kEmbeddedXmaReleaseSymbol = "rex_gpu_embedded_xma_release";
inline constexpr const char* kEmbeddedXmaReadMmioSymbol = "rex_gpu_embedded_xma_read_mmio";
inline constexpr const char* kEmbeddedXmaWriteMmioSymbol = "rex_gpu_embedded_xma_write_mmio";

using EmbeddedGpuInterruptCallback = void (*)(void* context, uint32_t callback,
                                               uint32_t source, uint32_t cpu,
                                               uint32_t callback_data);

struct EmbeddedGpuCreateInfo {
  uint32_t struct_size = 0;
  const char* backend = nullptr;
  uint8_t* virtual_membase = nullptr;
  uint8_t* physical_membase = nullptr;
  double refresh_rate_hz = 60.0;
  EmbeddedGpuInterruptCallback interrupt_callback = nullptr;
  void* interrupt_context = nullptr;
  // Original UTF-8 configuration path for relative resources and diagnostics.
  // This file must never be reopened by an embedded consumer.
  const char* config_path_utf8 = nullptr;
  // Absolute package root containing immutable runtime_data assets. The host
  // owns this identity; the plugin consumes the pointer synchronously.
  const char* asset_root_utf8 = nullptr;
  // Optional persistent shader/pipeline cache identity. The host chooses a
  // writable root already segregated by exact title executable and PC
  // settings; ReXGlue additionally validates translator/pipeline versions.
  // Both string pointers are consumed synchronously during creation.
  const char* cache_root_utf8 = nullptr;
  uint32_t title_id = 0;
  // Non-zero finishes storage loading before returning from creation, avoiding
  // first-use compilation races in the embedded title startup path.
  uint32_t shader_storage_blocking = 1;
  // Optional absolute UTF-8 directory for user-requested final guest-output
  // captures. The plugin copies the path during creation and creates the
  // directory only when a physical capture request is received.
  const char* screenshot_root_utf8 = nullptr;
  // Host-owned startup bytes, consumed synchronously during creation. Presence
  // is distinct from an empty document; absent config retains built-in values.
  const char* config_contents_utf8 = nullptr;
  uint64_t config_contents_size = 0;
  uint32_t config_present = 0;
  rex::memory::HostWriteCallbacks host_writes{};
  rex::graphics::pc_owned_camera_packet::Callbacks camera_packets{};
};

// Title-specific settings consumed by the static-recompilation host. ReXGlue
// remains the single parser and validator for the shared PC configuration.
struct EmbeddedGpuPcSettings {
  uint32_t struct_size = 0;
  float gameplay_fov_degrees = 86.0f;  // horizontal at 16:9 (V376; 86 = original)
  uint32_t trace_camera_state = 0;
  uint32_t keyboard_mouse_enabled = 0;
  uint32_t keyboard_mouse_user_index = 0;
};

// Host-native keyboard and relative-mouse state normalized to the Xbox 360
// gamepad contract. This is user-driven physical input, not injected guest
// state. The static-recompilation host merges it with the real XInput device
// only when keyboard/mouse support is explicitly enabled in PC configuration.
struct EmbeddedHostInputState {
  uint32_t struct_size = 0;
  uint32_t packet_number = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
};

// Native mouse look: raw relative mouse counts accumulated since the host's
// previous call, which the host feeds to the title's own look command, plus
// the user's sensitivity and Y inversion. active = 0 while keyboard/mouse is
// disabled, unfocused, not captured, or configured as the right-stick bridge.
struct EmbeddedMouseLook {
  uint32_t struct_size = 0;
  uint32_t active = 0;
  int32_t dx = 0;
  int32_t dy = 0;
  float sensitivity = 1.0f;
  uint32_t invert_y = 0;
};

struct GpuCreateInfo {
  uint32_t struct_size = 0;       // sizeof(GpuCreateInfo), set by the host
  const char* backend = nullptr;  // "d3d12", "vulkan", or "any"
};

// extern "C" exports every GPU plugin must provide:
//   uint32_t rex_gpu_abi_version(void);
//   rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
//                                                const GpuCreateInfo* info);
using GpuAbiVersionFn = uint32_t (*)();
using GpuCreateFn = IGraphicsSystem* (*)(uint32_t abi_version, const GpuCreateInfo* info);

// Loads rexgpu-<name> from the executable's directory and constructs its
// graphics system. Returns nullptr after logging a detailed error (missing
// file, missing exports, ABI mismatch, or factory failure). The library
// handle is retained for process lifetime; plugins are never unloaded.
std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name,
                                               std::string_view backend = "any");

}  // namespace rex::system
