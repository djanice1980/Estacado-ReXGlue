#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rex::graphics {
// Advisory wake only: never represents a GPU fence or guest comparison result.
// Snapshot BEFORE reading guest memory; a completion between read and wait then
// changes the generation and cannot be lost. Uncovered writers retain timeout.
class CpInterruptWakeup {
 public:
  uint64_t Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }
  void Notify() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++generation_;
    }
    changed_.notify_all();
  }
  bool Wait(uint64_t observed, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return generation_ != observed; });
  }
 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  uint64_t generation_ = 0;
};
inline CpInterruptWakeup cp_interrupt_wakeup;
}  // namespace rex::graphics
