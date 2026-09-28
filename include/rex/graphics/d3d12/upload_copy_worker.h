/**
 * @file        rex/graphics/d3d12/upload_copy_worker.h
 * @brief       Copies guest memory into D3D12 upload pages on a worker thread.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

// Command-processor copy stage (gpu option d3d12_async_upload_copies).
//
// D3D12SharedMemory::UploadRanges records the GPU copy from an upload page
// into the shared-memory buffer at once, in command order, but the CPU copy of
// guest memory into that upload page may run here instead of on the command
// processor thread. Every copy has a ticket (1, 2, ...). A copy only has to be
// complete
//   - before the submission that contains its GPU copy executes (the
//     submission job carries the ticket), and
//   - before the command processor makes anything guest-visible that tells
//     the title the GPU has consumed its data (fence and memory writes,
//     interrupts, swaps): until then the title may not touch that data, on
//     real hardware or here.
// One producer (the command processor thread), one consumer (this worker).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace rex::graphics::d3d12 {

class UploadCopyWorker {
 public:
  UploadCopyWorker() : thread_([this] { Run(); }) {}
  ~UploadCopyWorker() {
    stop_.store(true, std::memory_order_release);
    Wake();
    if (thread_.joinable()) thread_.join();
  }
  UploadCopyWorker(const UploadCopyWorker&) = delete;
  UploadCopyWorker& operator=(const UploadCopyWorker&) = delete;

  // Producer only. Returns the copy's ticket.
  uint64_t Enqueue(void* destination, const void* source, size_t bytes) {
    const uint64_t ticket = queued_ + 1;
    // Full queue: wait for the worker (bounded memory, keeps ticket order).
    while (ticket - completed_.load(std::memory_order_acquire) > kCapacity) Pause();
    jobs_[(ticket - 1) & (kCapacity - 1)] = {destination, source, bytes};
    queued_ = ticket;
    // Store then load, both sequentially consistent, against the worker's
    // sleeping_ store then published_ load: one side always sees the other.
    published_.store(ticket, std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_seq_cst)) Wake();
    return ticket;
  }

  // Producer only: the ticket of the last queued copy (0 before the first).
  uint64_t queued() const { return queued_; }
  uint64_t completed() const { return completed_.load(std::memory_order_acquire); }

  // Any thread: returns once every copy up to and including ticket is done.
  void Wait(uint64_t ticket) {
    for (uint32_t spins = 0; completed_.load(std::memory_order_acquire) < ticket; ++spins) {
      if (spins < 4096) {
        Pause();
      } else {
        std::this_thread::yield();
      }
    }
  }

 private:
  static constexpr uint64_t kCapacity = 4096;  // power of two
  struct Job {
    void* destination;
    const void* source;
    size_t bytes;
  };

  static void Pause() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
  }

  void Wake() {
    wake_.fetch_add(1, std::memory_order_release);
    wake_.notify_one();
  }

  void Run() {
    uint64_t done = 0;
    while (true) {
      const uint64_t available = published_.load(std::memory_order_acquire);
      if (done < available) {
        const Job& job = jobs_[done & (kCapacity - 1)];
        std::memcpy(job.destination, job.source, job.bytes);
        completed_.store(++done, std::memory_order_release);
        continue;
      }
      if (stop_.load(std::memory_order_acquire)) break;
      // Brief spin for the next copy of this burst, then sleep until the
      // producer publishes more (a draw's copies arrive microseconds apart).
      bool more = false;
      for (uint32_t spins = 0; spins < 2048 && !more; ++spins) {
        Pause();
        more = published_.load(std::memory_order_acquire) != done;
      }
      if (more) continue;
      const uint32_t observed = wake_.load(std::memory_order_acquire);
      sleeping_.store(true, std::memory_order_seq_cst);
      if (published_.load(std::memory_order_seq_cst) == done &&
          !stop_.load(std::memory_order_acquire)) {
        wake_.wait(observed, std::memory_order_acquire);
      }
      sleeping_.store(false, std::memory_order_release);
    }
  }

  Job jobs_[kCapacity] = {};
  uint64_t queued_ = 0;  // producer-owned
  std::atomic<uint64_t> published_{0};
  std::atomic<uint64_t> completed_{0};
  std::atomic<uint32_t> wake_{0};
  std::atomic<bool> sleeping_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;  // last: starts after the members above exist
};

}  // namespace rex::graphics::d3d12
