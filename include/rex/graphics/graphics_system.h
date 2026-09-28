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

#pragma once

#include "rex/system/function_dispatcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/graphics/register_file.h>
#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>
#include <rex/thread/mutex.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>

// Forward declarations
namespace rex {
class Memory;
namespace stream {
class ByteStream;
}  // namespace stream
}  // namespace rex

namespace rex::ui {
class WindowedAppContext;
}  // namespace rex::ui

namespace rex::graphics {

class CommandProcessor;

class GraphicsSystem : public system::IGraphicsSystem {
 public:
  virtual ~GraphicsSystem();

  virtual std::string name() const = 0;

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  system::KernelState* kernel_state() const { return kernel_state_; }
  ::rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  ::rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override;
  // The presentation thread ends before Shutdown (the embedded title path
  // joins it first) and its app context is a local of that thread: forget it
  // there, so Shutdown releases the presenter on its own thread instead of
  // calling into the destroyed context (V380: rexruntime.dll+0x100674 access
  // violation after every graceful stop).
  void ReleaseAppContext() { app_context_ = nullptr; }
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         system::KernelState* kernel_state) override;
  // Initializes only the GPU subsystem for a static-recompilation host. The
  // supplied Memory adopts host-owned mappings and no ReXGlue kernel,
  // dispatcher, filesystem, or object system is constructed.
  X_STATUS SetupEmbeddedGuestGpu(
      memory::Memory* memory, double refresh_rate_hz,
      std::function<void(uint32_t callback, uint32_t source, uint32_t cpu,
                         uint32_t callback_data)>
          interrupt_dispatch);
  bool has_presentation() const override { return presenter_ != nullptr; }
  void Shutdown() override;

  // May be called from any thread any number of times, even during recovery
  // from a device loss.
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  RegisterFile* register_file() { return &register_file_; }
  CommandProcessor* command_processor() const { return command_processor_.get(); }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  uint32_t ReadRegisterEmbedded(uint32_t addr) { return ReadRegister(addr); }
  void WriteRegisterEmbedded(uint32_t addr, uint32_t value) { WriteRegister(addr, value); }
  void NotifyPhysicalMemoryWrite(uint32_t address, uint32_t length);
  // Host write of CPU-side data made inside the plugin (any thread, after the
  // write): marked lock-free, published on the command processor thread.
  void NotifyHostWrite(uint32_t address, uint32_t length);

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  void RequestFrameTrace();
  void BeginTracing();
  void EndTracing();

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(::rex::stream::ByteStream* stream);
  bool Restore(::rex::stream::ByteStream* stream);

 protected:
  GraphicsSystem();

  // Backends build their provider here. Called lazily from either setup
  // entry point; with_presentation is false only on headless guest-GPU paths.
  virtual void CreateProvider(bool with_presentation) = 0;

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static uint32_t ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                 uint32_t value);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  void MarkVblank();

  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  system::KernelState* kernel_state_ = nullptr;
  ::rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<::rex::ui::GraphicsProvider> provider_;
  bool provider_supports_presentation_ = false;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::atomic<bool> vsync_worker_running_;
  system::object_ref<system::XHostThread> vsync_worker_thread_;
  std::thread embedded_vsync_worker_thread_;
  std::function<void(uint32_t, uint32_t, uint32_t, uint32_t)> embedded_interrupt_dispatch_;

  RegisterFile register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

  bool paused_ = false;

 private:
  std::unique_ptr<::rex::ui::Presenter> presenter_;

  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace rex::graphics
