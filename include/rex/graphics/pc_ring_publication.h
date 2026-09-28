#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace rex::graphics::pc_ring_publication {

// Caller serializes CPU publication and CP consumption. This does not read or
// modify guest memory, publish a write pointer, or advance the guest read pointer.
// A ticket identifies one actual root-ring publication, not a child allocation.
class State {
 public:
  static constexpr uint32_t kCapacity = 1024;
  void Reset(uint32_t ring_dwords) noexcept {
    Disable();
    if (ring_dwords < 2 || (ring_dwords & (ring_dwords - 1))) return;
    ring_dwords_ = ring_dwords;
    enabled_ = sequence_ != std::numeric_limits<uint64_t>::max();
  }
  void Disable() noexcept {
    enabled_ = false;
    first_ = count_ = ring_dwords_ = 0;
    read_ = written_ = 0;
  }
  bool Enabled() const noexcept { return enabled_; }

  // Called before the actual CP write pointer is published. Equal pointers
  // produce no range and no new identity. An invalid/overrun queue fails closed.
  uint64_t Publish(uint32_t write_index) noexcept {
    if (!enabled_) return 0;
    if (write_index >= ring_dwords_) return Fail();
    const uint32_t mask = ring_dwords_ - 1;
    const uint32_t words = (write_index - uint32_t(written_ & mask)) & mask;
    if (!words) return 0;
    if (count_ == kCapacity || words >= ring_dwords_ - (written_ - read_) ||
        written_ > std::numeric_limits<uint64_t>::max() - words ||
        sequence_ == std::numeric_limits<uint64_t>::max()) return Fail();
    const uint64_t ticket = ++sequence_;
    written_ += words;
    ranges_[(first_ + count_) % kCapacity] = {written_, ticket};
    ++count_;
    return ticket;
  }

  // Claim only a complete root packet contained in one publication. Crossing a
  // publication boundary consumes bookkeeping but returns unknown (zero).
  uint64_t Consume(uint32_t read_index, uint32_t packet_words) noexcept {
    if (!enabled_) return 0;
    if (!packet_words || read_index >= ring_dwords_ ||
        read_index != (read_ & (ring_dwords_ - 1)) ||
        packet_words > written_ - read_ || !count_) return Fail();
    const uint64_t end = read_ + packet_words;
    const uint64_t ticket = end <= ranges_[first_].end ? ranges_[first_].ticket : 0;
    read_ = end;
    while (count_ && ranges_[first_].end <= read_) {
      first_ = (first_ + 1) % kCapacity;
      --count_;
    }
    return ticket;
  }

 private:
  struct Range { uint64_t end = 0, ticket = 0; };
  uint64_t Fail() noexcept { Disable(); return 0; }
  std::array<Range, kCapacity> ranges_{};
  uint64_t read_ = 0, written_ = 0, sequence_ = 0;
  uint32_t ring_dwords_ = 0, first_ = 0, count_ = 0;
  bool enabled_ = false;
};

inline uint32_t PacketWords(uint32_t header) noexcept {
  if (!header || (header >> 30) == 2) return 1;
  if ((header >> 30) == 1) return 3;
  return ((header >> 16) & 0x3FFFu) + 2;
}

}  // namespace rex::graphics::pc_ring_publication
