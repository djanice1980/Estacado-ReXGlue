#pragma once
#include <cstdint>

namespace rex::memory {
// Supplied once by the embedded owner before workers start. begin must acquire
// the owner's payload guard before returning; end releases it on the same thread.
// A null pair is the ordinary untracked path. The host retains callback/context
// lifetime until all plugin workers have stopped. Callbacks must not throw.
struct HostWriteCallbacks {
  void* context = nullptr;
  void* (*begin)(void*, uint32_t physical, uint32_t bytes) noexcept = nullptr;
  void (*end)(void*, void* token) noexcept = nullptr;
  bool Valid() const noexcept { return (begin == nullptr) == (end == nullptr); }
};

class HostWriteScope {
 public:
  HostWriteScope(HostWriteCallbacks callbacks, uint32_t physical, uint32_t bytes) noexcept
      : callbacks_(callbacks) {
    if (callbacks_.begin && callbacks_.end && bytes) {
      active_ = true;
      token_ = callbacks_.begin(callbacks_.context, physical, bytes);
    }
  }
  ~HostWriteScope() {
    // A null token is opaque too: every invoked begin gets exactly one end.
    if (active_) callbacks_.end(callbacks_.context, token_);
  }
  HostWriteScope(const HostWriteScope&) = delete;
  HostWriteScope& operator=(const HostWriteScope&) = delete;
 private:
  HostWriteCallbacks callbacks_;
  void* token_ = nullptr;
  bool active_ = false;
};
}  // namespace rex::memory
