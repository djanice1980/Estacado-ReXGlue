#pragma once

namespace rex::graphics::shared_memory_watch_policy {
// A handled write is scoped to a single GPU notification. CPU writes and
// nested writes without their own explicit handled identity always notify.
inline bool ShouldNotify(const void* watch, const void* handled, bool gpu) {
  return !gpu || !handled || watch != handled;
}
}
