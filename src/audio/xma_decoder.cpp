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

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/string/buffer.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" {
#include "libavutil/log.h"
}  // extern "C"

REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, "Audio", "Verbose FFmpeg output (debug and above)");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal XmaDecoder handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc
//
// XMA details:
// https://devel.nuclex.org/external/svn/directx/trunk/include/xma2defs.h
// https://github.com/gdawg/fsbext/blob/master/src/xma_header.h
//
// XAudio2 uses XMA under the covers, and seems to map with the same
// restrictions of frame/subframe/etc:
// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.xaudio2.xaudio2_buffer(v=vs.85).aspx
//
// XMA contexts are 64b in size and tight bitfields. They are in physical
// memory not usually available to games. Games will use MmMapIoSpace to get
// the 64b pointer in user memory so they can party on it. If the game doesn't
// do this, it's likely they are either passing the context to XAudio or
// using the XMA* functions.

namespace rex::audio {

constexpr bool kEmbeddedHotPathDiagnosticsEnabled = false;

namespace {

constexpr uint32_t kXmaContextErrorDwordOffset = 2 * sizeof(uint32_t);
constexpr uint32_t kXmaContextErrorStatusShift = 26;
constexpr uint32_t kXmaContextErrorStatusMask = 0x1F;
constexpr size_t kEmbeddedXmaContextCount = 320;

struct EmbeddedXmaErrorTraceState {
  std::array<std::atomic<uint8_t>, kEmbeddedXmaContextCount> last_status{};
  std::array<std::atomic<uint64_t>, kEmbeddedXmaContextCount> observations{};
};

constexpr uint8_t DecodeEmbeddedXmaErrorStatus(uint32_t guest_error_dword) {
  const uint32_t host_error_dword = rex::byte_swap(guest_error_dword);
  return static_cast<uint8_t>((host_error_dword >> kXmaContextErrorStatusShift) &
                              kXmaContextErrorStatusMask);
}

static_assert(DecodeEmbeddedXmaErrorStatus(0x00000010) == 4);
static_assert(DecodeEmbeddedXmaErrorStatus(0x0000007C) == 31);

uint8_t ReadEmbeddedXmaErrorStatus(memory::Memory* memory, uint32_t guest_ptr) {
  // XMA_CONTEXT_DATA DWORD 2 is stored big-endian in guest memory. Reading
  // only that word avoids copying and swapping all 64 bytes on every kick
  // solely for an error diagnostic that is normally inactive.
  uint32_t guest_error_dword = 0;
  const uint8_t* context_ptr = memory->TranslateVirtual(guest_ptr);
  std::memcpy(&guest_error_dword, context_ptr + kXmaContextErrorDwordOffset,
              sizeof(guest_error_dword));
  return DecodeEmbeddedXmaErrorStatus(guest_error_dword);
}

bool ShouldTraceEmbeddedXmaError(EmbeddedXmaErrorTraceState& trace_state, uint32_t context_id,
                                 uint8_t error_status) {
  auto& last_status = trace_state.last_status[context_id];
  const uint8_t previous = last_status.load(std::memory_order_relaxed);
  if (!error_status) {
    if (previous) {
      last_status.store(0, std::memory_order_relaxed);
      trace_state.observations[context_id].store(0, std::memory_order_relaxed);
    }
    return false;
  }

  uint64_t observation = 0;
  if (previous != error_status) {
    last_status.store(error_status, std::memory_order_relaxed);
    trace_state.observations[context_id].store(1, std::memory_order_relaxed);
    observation = 1;
  } else {
    observation = trace_state.observations[context_id].fetch_add(1, std::memory_order_relaxed) + 1;
  }

  // Preserve immediate error visibility, then remain bounded if the guest
  // repeatedly kicks a context while the same error persists.
  return observation <= 4 || !(observation & (observation - 1));
}

}  // namespace

XmaDecoder::XmaDecoder(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()), function_dispatcher_(function_dispatcher) {}

XmaDecoder::XmaDecoder(memory::Memory* memory) : memory_(memory) {}

XmaDecoder::~XmaDecoder() = default;

void av_log_callback(void* avcl, int level, const char* fmt, va_list va) {
  if (!REXCVAR_GET(ffmpeg_verbose) && level > AV_LOG_WARNING) {
    return;
  }

  string::StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.to_string_view();

  switch (level) {
    case AV_LOG_ERROR:
      REXAPU_ERROR("ffmpeg: {}", msg);
      break;
    case AV_LOG_WARNING:
      REXAPU_WARN("ffmpeg: {}", msg);
      break;
    case AV_LOG_INFO:
      REXAPU_INFO("ffmpeg: {}", msg);
      break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG:
    default:
      REXAPU_DEBUG("ffmpeg: {}", msg);
      break;
  }
}

X_STATUS XmaDecoder::Setup(system::KernelState* kernel_state) {
  // Setup ffmpeg logging callback
  av_log_set_callback(av_log_callback);

  // Register APU/XMA MMIO handlers
  // XMA registers are at 0x7FEA0000-0x7FEAFFFF
  memory()->AddVirtualMappedRange(
      0x7FEA0000,  // base address
      0xFFFF0000,  // mask
      0x0000FFFF,  // size (64KB)
      this,        // context (XmaDecoder*)
      reinterpret_cast<runtime::MMIOReadCallback>(MMIOReadRegisterThunk),
      reinterpret_cast<runtime::MMIOWriteCallback>(MMIOWriteRegisterThunk));
  REXAPU_DEBUG("XMA: Registered MMIO handlers at 0x7FEA0000-0x7FEAFFFF");

  // Setup XMA context data.
  // The Xbox 360 kernel allocates the contexts with X_PAGE_NOCACHE |
  // X_PAGE_READWRITE and writes MmGetPhysicalAddress for the address to the
  // register.
  context_data_first_ptr_ = memory()->SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA) * kContextCount, 256,
                                                      memory::kSystemHeapPhysical);
  owns_context_data_ = true;
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XmaRegister::ContextArrayAddress] =
      memory()->GetPhysicalAddress(context_data_first_ptr_);

  // Setup XMA contexts.
  for (size_t i = 0; i < kContextCount; ++i) {
    uint32_t guest_ptr = context_data_first_ptr_ + i * sizeof(XMA_CONTEXT_DATA);
    XmaContext& context = contexts_[i];
    if (context.Setup(i, memory(), guest_ptr)) {
      assert_always();
    }
  }
  register_file_[XmaRegister::NextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);

  worker_running_ = true;
  work_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(work_event_);
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("XMA Decoder");

  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

X_STATUS XmaDecoder::SetupExternal(uint32_t context_data_first_ptr) {
  if (!context_data_first_ptr || (context_data_first_ptr & 0xFF) ||
      context_data_first_ptr > UINT32_MAX - sizeof(XMA_CONTEXT_DATA) * kContextCount) {
    return X_STATUS_INVALID_PARAMETER;
  }

  av_log_set_callback(av_log_callback);
  context_data_first_ptr_ = context_data_first_ptr;
  context_data_last_ptr_ = context_data_first_ptr_ + sizeof(XMA_CONTEXT_DATA) * kContextCount - 1;
  owns_context_data_ = false;
  register_file_[XmaRegister::ContextArrayAddress] =
      memory()->GetPhysicalAddress(context_data_first_ptr_);
  if (register_file_[XmaRegister::ContextArrayAddress] == UINT32_MAX) {
    context_data_first_ptr_ = 0;
    context_data_last_ptr_ = 0;
    return X_STATUS_INVALID_PARAMETER;
  }

  {
    auto host_write = memory()->GuardVirtualWrite(context_data_first_ptr_,
        sizeof(XMA_CONTEXT_DATA) * kContextCount);
    std::memset(memory()->TranslateVirtual(context_data_first_ptr_), 0,
                sizeof(XMA_CONTEXT_DATA) * kContextCount);
  }
  for (size_t i = 0; i < kContextCount; ++i) {
    const uint32_t guest_ptr = context_data_first_ptr_ + uint32_t(i * sizeof(XMA_CONTEXT_DATA));
    if (contexts_[i].Setup(static_cast<uint32_t>(i), memory(), guest_ptr)) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  register_file_[XmaRegister::NextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);
  worker_running_ = true;
  work_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  if (!work_event_)
    return X_STATUS_UNSUCCESSFUL;
  external_worker_thread_ = std::thread([this]() {
    rex::thread::set_current_thread_name("XMA Decoder");
    std::fprintf(stderr, "REX_EMBEDDED_XMA_WORKER_START native_thread=%u\n",
                 rex::thread::current_thread_system_id());
    std::fflush(stderr);
    WorkerThreadMain();
    std::fprintf(stderr, "REX_EMBEDDED_XMA_WORKER_STOP native_thread=%u\n",
                 rex::thread::current_thread_system_id());
    std::fflush(stderr);
  });
  return X_STATUS_SUCCESS;
}

void XmaDecoder::WorkerThreadMain() {
  while (worker_running_) {
    // Okay, let's loop through XMA contexts to find ones we need to decode!
    bool did_work = false;
    for (uint32_t n = 0; n < kContextCount && worker_running_; n++) {
      XmaContext& context = contexts_[n];
      bool worked = context.Work();
      if (worked) {
        context.SignalWorkDone();
        PROFILE_XMA_FRAME_DECODED();
      }
      did_work = did_work || worked;
    }

    if (paused_) {
      pause_fence_.Signal();
      resume_fence_.Wait();
    }

    if (did_work) {
      continue;
    }
    // No work done this iteration, block until signaled.
    rex::thread::Wait(work_event_.get(), false);
  }
}

void XmaDecoder::Shutdown() {
  if (!worker_thread_ && !external_worker_thread_.joinable()) {
    return;
  }

  if (external_worker_thread_.joinable()) {
    std::fprintf(stderr, "REX_EMBEDDED_XMA_SHUTDOWN_BEGIN\n");
    std::fflush(stderr);
  }

  worker_running_ = false;

  if (work_event_) {
    work_event_->Set();
  }

  if (paused_) {
    Resume();
  }

  if (external_worker_thread_.joinable()) {
    external_worker_thread_.join();
    std::fprintf(stderr, "REX_EMBEDDED_XMA_SHUTDOWN_COMPLETE\n");
    std::fflush(stderr);
  } else {
    // Wait up to 2 seconds for the kernel-owned worker to exit gracefully.
    auto result =
        rex::thread::Wait(worker_thread_->thread(), false, std::chrono::milliseconds(2000));
    if (result == rex::thread::WaitResult::kTimeout) {
      REXAPU_WARN("XMA: Worker thread did not exit within 2s, abandoning");
    }
    worker_thread_.reset();
  }

  if (context_data_first_ptr_ && owns_context_data_) {
    memory()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
  owns_context_data_ = false;
}

int XmaDecoder::GetContextId(uint32_t guest_ptr) {
  static_assert_size(XMA_CONTEXT_DATA, 64);
  if (guest_ptr < context_data_first_ptr_ || guest_ptr > context_data_last_ptr_) {
    return -1;
  }
  assert_zero(guest_ptr & 0x3F);
  return (guest_ptr - context_data_first_ptr_) >> 6;
}

uint32_t XmaDecoder::AllocateContext() {
  size_t index = context_bitmap_.Acquire();
  if (index == -1) {
    // Out of contexts.
    return 0;
  }

  XmaContext& context = contexts_[index];
  assert_false(context.is_allocated());
  context.set_is_allocated(true);
  return context.guest_ptr();
}

void XmaDecoder::ReleaseContext(uint32_t guest_ptr) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = contexts_[context_id];
  assert_true(context.is_allocated());
  context.Release();
  context_bitmap_.Release(context_id);
}

bool XmaDecoder::BlockOnContext(uint32_t guest_ptr, bool poll) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = contexts_[context_id];
  return context.Block(poll);
}

uint32_t XmaDecoder::ReadRegister(uint32_t addr) {
  auto r = (addr & 0xFFFF) / 4;

  assert_true(r < XmaRegisterFile::kRegisterCount);

  switch (r) {
    case XmaRegister::ContextArrayAddress:
      break;
    case XmaRegister::CurrentContextIndex: {
      // 0606h (1818h) is rotating context processing # set to hardware ID of
      // context being processed.
      // If bit 200h is set, the locking code will possibly collide on hardware
      // IDs and error out, so we should never set it (I think?).
      uint32_t& current_context_index = register_file_[XmaRegister::CurrentContextIndex];
      uint32_t& next_context_index = register_file_[XmaRegister::NextContextIndex];
      // To prevent games from seeing a stuck XMA context, return a rotating
      // number.
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXAPU_DEBUG("XMA: Read from unhandled register ({:04X}, {})", r, register_info->name);
      } else {
        REXAPU_DEBUG("XMA: Read from unknown register ({:04X})", r);
      }
      break;
  }

  return rex::byte_swap(register_file_[r]);
}

void XmaDecoder::WriteRegister(uint32_t addr, uint32_t value) {
  SCOPE_profile_cpu_f("apu");

  const uint32_t guest_value = value;
  uint32_t r = (addr & 0xFFFF) / 4;
  value = rex::byte_swap(value);

  if (!function_dispatcher_) {
    static std::atomic<uint64_t> embedded_write_ordinal = 0;
    const uint64_t ordinal = ++embedded_write_ordinal;
    if (kEmbeddedHotPathDiagnosticsEnabled && (ordinal <= 64 || !(ordinal & (ordinal - 1)))) {
      std::fprintf(stderr,
                   "REX_EMBEDDED_XMA_MMIO_WRITE ordinal=%llu address=0x%08X register=0x%04X "
                   "guest=0x%08X decoded=0x%08X\n",
                   static_cast<unsigned long long>(ordinal), addr, r, guest_value, value);
      std::fflush(stderr);
    }
  }

  assert_true(r < XmaRegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XmaRegister::Context0Kick && r <= XmaRegister::Context9Kick) {
    // Context kick command.
    // This will kick off the given hardware contexts.
    // Basically, this kicks the SPU and says "hey, decode that audio!"
    // XMAEnableContext

    // The context ID is a bit in the range of the entire context array.
    uint32_t base_context_id = (r - XmaRegister::Context0Kick) * 32;
    uint32_t kicked_value = value;
    static std::atomic<uint64_t> embedded_kick_ordinal = 0;
    static EmbeddedXmaErrorTraceState embedded_error_trace_state;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        if (!function_dispatcher_) {
          const uint64_t ordinal = ++embedded_kick_ordinal;
          const uint8_t error_status = ReadEmbeddedXmaErrorStatus(memory(), context.guest_ptr());
          if (ShouldTraceEmbeddedXmaError(embedded_error_trace_state, context_id, error_status)) {
            const XMA_CONTEXT_DATA data(memory()->TranslateVirtual(context.guest_ptr()));
            std::fprintf(stderr,
                         "REX_EMBEDDED_XMA_KICK ordinal=%llu context=%u guest=0x%08X allocated=%u "
                         "enabled=%u input0=0x%08X packets0=%u valid0=%u input1=0x%08X "
                         "packets1=%u valid1=%u current=%u read_bits=%u output=0x%08X blocks=%u "
                         "valid=%u read=%u write=%u rate=%u stereo=%u error=%u\n",
                         static_cast<unsigned long long>(ordinal), context_id, context.guest_ptr(),
                         context.is_allocated() ? 1u : 0u, context.is_enabled() ? 1u : 0u,
                         data.input_buffer_0_ptr, data.input_buffer_0_packet_count,
                         data.input_buffer_0_valid, data.input_buffer_1_ptr,
                         data.input_buffer_1_packet_count, data.input_buffer_1_valid,
                         data.current_buffer, data.input_buffer_read_offset, data.output_buffer_ptr,
                         data.output_buffer_block_count, data.output_buffer_valid,
                         data.output_buffer_read_offset, data.output_buffer_write_offset,
                         data.sample_rate, data.is_stereo, data.error_status);
            std::fflush(stderr);
          }
        }
        context.Enable();
      }
    }
    // Decode inline. waiting on the worker sweep stalls the realtime
    // audio thread 50-100ms during kick bursts.
    for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
      if (kicked_value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        const bool worked = context.Work();
        if (!function_dispatcher_) {
          const uint64_t ordinal = embedded_kick_ordinal.load();
          const uint8_t error_status = ReadEmbeddedXmaErrorStatus(memory(), context.guest_ptr());
          if (ShouldTraceEmbeddedXmaError(embedded_error_trace_state, context_id, error_status)) {
            const XMA_CONTEXT_DATA data(memory()->TranslateVirtual(context.guest_ptr()));
            std::fprintf(stderr,
                         "REX_EMBEDDED_XMA_KICK_RESULT ordinal=%llu context=%u worked=%u "
                         "enabled=%u valid0=%u valid1=%u current=%u read_bits=%u "
                         "output_valid=%u output_read=%u output_write=%u error=%u\n",
                         static_cast<unsigned long long>(ordinal), context_id, worked ? 1u : 0u,
                         context.is_enabled() ? 1u : 0u, data.input_buffer_0_valid,
                         data.input_buffer_1_valid, data.current_buffer,
                         data.input_buffer_read_offset, data.output_buffer_valid,
                         data.output_buffer_read_offset, data.output_buffer_write_offset,
                         data.error_status);
            std::fflush(stderr);
          }
        }
        if (worked) {
          context.SignalWorkDone();
        }
      }
    }
    work_event_->Set();
  } else if (r >= XmaRegister::Context0Lock && r <= XmaRegister::Context9Lock) {
    // Context lock command.
    // This requests a lock by flagging the context.
    // XMADisableContext
    uint32_t base_context_id = (r - XmaRegister::Context0Lock) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Disable();
        // [XMA fix] Added Block(false) after Disable(). Without this, the game
        // could call XMADisableContext and start modifying the context struct
        // while a decode was still in progress on the worker thread. Block()
        // waits for the context mutex to be free (poll=false means wait, not spin).
        context.Block(false);
      }
    }
    // Signal the decoder thread to start processing.
    // work_event_->Set();
  } else if (r >= XmaRegister::Context0Clear && r <= XmaRegister::Context9Clear) {
    // Context clear command.
    // This will reset the given hardware contexts.
    uint32_t base_context_id = (r - XmaRegister::Context0Clear) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        XmaContext& context = contexts_[context_id];
        context.Clear();
      }
    }
  } else {
    // 0601h (1804h) is written to with 0x02000000 and 0x03000000 around a lock
    // operation
    switch (r) {
      default: {
        const auto register_info = register_file_.GetRegisterInfo(r);
        if (register_info) {
          REXAPU_DEBUG("XMA: Write to unhandled register ({:04X}, {}): {:08X}", r,
                       register_info->name, value);
        } else {
          REXAPU_DEBUG("XMA: Write to unknown register ({:04X}): {:08X}", r, value);
        }
        break;
      }
#pragma warning(suppress : 4065)
    }
  }
}

void XmaDecoder::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  if (work_event_) {
    work_event_->Set();
  }
  pause_fence_.Wait();
}

void XmaDecoder::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_fence_.Signal();
}

}  // namespace rex::audio
