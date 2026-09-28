#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace rex::graphics {
// Host-only evidence. One embedded GPU per process. Never read as guest state
// or used to signal completion. Tokens prevent late records crossing windows.
class CpInterruptTiming {
 public:
  struct Record {
    uint32_t kind = 0, source = 0, address = 0, before = 0, after = 0;
    uint64_t enqueue = 0, dispatch = 0, returned = 0;
  };
  struct Batch {
    std::array<Record, 64> records{};
    uint32_t count = 0, overflow = 0;
  };
  uint64_t Token() const { return token_.load(std::memory_order_acquire); }
  void Begin(uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    batch_ = {};
    token_.store(token, std::memory_order_release);
  }
  void Add(uint64_t token, Record record) {
    if (!token || Token() != token) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (Token() != token) return;
    if (batch_.count < batch_.records.size()) batch_.records[batch_.count++] = record;
    else ++batch_.overflow;
  }
  Batch Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    token_.store(0, std::memory_order_release);
    return batch_;
  }
 private:
  std::atomic<uint64_t> token_{};
  std::mutex mutex_;
  Batch batch_{};
};
inline CpInterruptTiming cp_interrupt_timing;
}  // namespace rex::graphics
