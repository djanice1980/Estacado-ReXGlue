#pragma once
#include <cstdint>
#include <limits>

namespace rex::graphics::pc_command_execution {

// Identity of actual CP execution, never a guest allocation or CPU publication.
// Replaying identical bytes at the same address produces a different identity.
struct Token {
  uint64_t buffer = 0;
  uint64_t parent = 0;
  uint64_t packet = 0;
  uint64_t parent_packet = 0;
  uint32_t depth = 0;
  uint64_t root_publication = 0;
  bool Valid() const noexcept { return buffer && packet && depth; }
};

// Owned by the command processor thread. IDs are never recycled by invalidation.
class State {
 public:
  class Scope {
   public:
    explicit Scope(State& state) noexcept
        : state_(state), saved_(state.current_), epoch_(state.epoch_) {
      const auto id = state.Next();
      ++state.scope_depth_;
      state.current_ = id && state.scope_depth_ <= 64
          ? Token{id, saved_.buffer, 0, saved_.packet, state.scope_depth_,
                  saved_.root_publication} : Token{};
    }
    ~Scope() {
      --state_.scope_depth_;
      state_.current_ = epoch_ == state_.epoch_ ? saved_ : Token{};
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
   private:
    State& state_;
    Token saved_;
    uint64_t epoch_;
  };

  void BeginPacket() noexcept {
    current_.packet = current_.buffer ? Next() : 0;
  }
  Token Current() const noexcept { return current_.Valid() ? current_ : Token{}; }
  void SetRootPublication(uint64_t ticket) noexcept {
    current_.root_publication = ticket;
  }
  void Invalidate() noexcept {
    current_ = {};
    // Exhaustion permanently disables metadata rather than recycling identity.
    if (epoch_ == std::numeric_limits<uint64_t>::max()) exhausted_ = true;
    else ++epoch_;
  }

 private:
  uint64_t Next() noexcept {
    if (exhausted_ || sequence_ == std::numeric_limits<uint64_t>::max()) {
      exhausted_ = true;
      return 0;
    }
    return ++sequence_;
  }
  Token current_{};
  uint64_t sequence_ = 0;
  uint64_t epoch_ = 0;
  uint32_t scope_depth_ = 0;
  bool exhausted_ = false;
};

}  // namespace rex::graphics::pc_command_execution
