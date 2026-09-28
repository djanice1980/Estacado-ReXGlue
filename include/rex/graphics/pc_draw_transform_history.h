#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <rex/graphics/pc_sparse_projection.h>
#include <rex/graphics/pc_projection_depth.h>
#include <rex/graphics/pc_constant_writer.h>

namespace rex::graphics::pc_draw_transform_history {

// Exact submitted inputs, not a camera matrix or a persistent instance ID.
// Never match preceding objects by array position, addresses or equal matrices.
struct Draw {
  uint64_t vertex_shader = 0;
  // c0..3 raster rows, c7 position offset, c12..15 current view-affine rows.
  // Native c16..19 are filtered blur transforms and deliberately excluded.
  std::array<uint32_t, 36> constants{};
  // Last observed writes matching these actual bound words. Source addresses
  // may be reused; sequences identify writes only, never persistent instances.
  std::array<pc_constant_writer::Record, 36> writers{};
  std::array<uint32_t, 6> viewport{};
  uint32_t window_offset = 0, scissor_tl = 0, scissor_br = 0;
  uint32_t vte = 0, clip = 0, surface = 0, depth = 0;

  bool CopyOwnedCamera(pc_owned_camera_packet::Source& output) const noexcept {
    // c12..15 are words20..35 in the exact bound upload. Require one complete
    // native group, not a mixture of individually plausible register values.
    const auto& first = writers[20].camera_source;
    for (uint32_t i = 0; i < 16; ++i) {
      const auto& writer = writers[20 + i];
      const auto& source = writer.camera_source;
      if (!writer.sequence || writer.value != constants[20 + i] ||
          !writer.execution.Valid() || !source.Covers(0x4030 + i) ||
          source.packet != first.packet || source.constant != first.constant ||
          source.publication != first.publication || source.item != first.item ||
          writer.execution.buffer != writers[20].execution.buffer ||
          writer.execution.packet != writers[20].execution.packet ||
          std::memcmp(source.camera_current, first.camera_current, sizeof(first.camera_current)))
        return false;
    }
    output = first;
    return true;
  }

  static constexpr bool ReviewedShader(uint64_t shader) noexcept {
    return shader == UINT64_C(0x81D611665A691E95) ||
           shader == UINT64_C(0xAE58BEF95D8148D5) ||
           shader == UINT64_C(0x87355DB0803F82C2) ||
           shader == UINT64_C(0xFA91501A8940251D) ||
           shader == UINT64_C(0xEBDE7C89E2B999EF) ||
           shader == UINT64_C(0x9C15826774B8A48D);
  }
  bool ReadUpload(const void* upload, uint32_t bytes, uint32_t count,
                  const uint64_t* bitmap) noexcept {
    constants = {};
    writers = {};
    if (!ReviewedShader(vertex_shader) || !upload || !bitmap || count > 256 ||
        bytes < count * 16) return false;
    uint32_t dense = 0, selected = 0;
    for (uint32_t reg = 0; reg < 256; ++reg) {
      if (!(bitmap[reg / 64] & (UINT64_C(1) << (reg % 64)))) continue;
      if (dense >= count) return false;
      if (reg < 4 || reg == 7 || (reg >= 12 && reg <= 15)) {
        uint32_t words[4];
        std::memcpy(words, static_cast<const uint8_t*>(upload) + dense * 16, 16);
        for (uint32_t word : words)
          if ((word & 0x7F800000u) == 0x7F800000u) return false;
        std::memcpy(constants.data() + selected * 4, words, 16);
        ++selected;
      }
      ++dense;
    }
    return dense == count && selected == 9;
  }
};

struct Frame {
  static constexpr uint32_t kMaximumDraws = 128;
  uint64_t frame = 0;
  uint32_t count = 0;
  bool failed = false, sealed = false;
  pc_sparse_projection::Projection projection;
  std::array<Draw, kMaximumDraws> draws{};

  // Reset logical membership without clearing or publishing stale array tails.
  void Reset() noexcept {
    frame = 0; count = 0; failed = sealed = false; projection = {};
  }
  void Add(uint64_t id, const Draw& draw, bool valid) noexcept {
    if (!frame) frame = id;
    if (!id || frame != id || sealed || !valid || count == kMaximumDraws) {
      failed = true;
      return;
    }
    const auto extracted = pc_sparse_projection::Extract(draw.constants);
    if (!count) projection = extracted;
    else if (!extracted.valid || !projection.valid || extracted.words != projection.words ||
             draw.viewport != draws[0].viewport || draw.vte != draws[0].vte ||
             draw.clip != draws[0].clip) projection = {};
    draws[count++] = draw;
  }
  void Seal(uint64_t id) noexcept {
    if (!id || frame != id || sealed || !count) failed = true;
    sealed = true;
  }
  bool Valid(uint64_t id) const noexcept {
    return id && frame == id && count && sealed && !failed;
  }
  bool SameProjection(const Frame& preceding) const noexcept {
    return Valid(frame) && preceding.Valid(preceding.frame) &&
           preceding.frame + 1 == frame && projection.valid && preceding.projection.valid &&
           projection.words == preceding.projection.words &&
           draws[0].viewport == preceding.draws[0].viewport &&
           draws[0].vte == preceding.draws[0].vte && draws[0].clip == preceding.draws[0].clip;
  }
  pc_projection_depth::Parameters ProjectionDepth() const noexcept {
    if (!Valid(frame)) return {};
    return pc_projection_depth::Derive(projection, draws[0].viewport,
                                       draws[0].vte, draws[0].clip);
  }
};

// Observation only: at most three later committed frames after the first
// image-proof series. Never changes retention, validity or native frame timing.
struct FollowupSamples {
  uint64_t last_proof = 0;
  uint32_t emitted = 0;
  bool armed = false, closed = false;
  bool Select(uint64_t frame, bool proof, bool committed) noexcept {
    if (closed) return false;
    if (armed && frame < last_proof) { closed = true; return false; }
    if (proof) {
      if (!emitted) { last_proof = frame; armed = true; }
      return false;
    }
    if (!armed || !committed || frame <= last_proof) return false;
    constexpr uint64_t offsets[] = {16, 64, 256};
    if (frame - last_proof < offsets[emitted]) return false;
    if (++emitted == 3) closed = true;
    return true;
  }
};

}  // namespace rex::graphics::pc_draw_transform_history
