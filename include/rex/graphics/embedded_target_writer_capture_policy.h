#pragma once

#include <cstdint>

namespace rex::graphics::embedded_target_writer_capture_policy {

// Diagnostic selectors, not inferred scene semantics. The default count is
// zero. One selected frame may emit a before-image plus at most eight sparse
// after-images. A different frame can never continue a partial capture chain.
struct Config {
  uint32_t surface = 0;
  uint32_t color = 0;
  uint32_t scissor_br = 0;
  uint32_t minimum_draw = 0;
  uint32_t first = 1;
  uint32_t stride = 256;
  uint32_t count = 0;
  uint64_t pixel_shader_hash = 0;
  // Exact shader-qualified pairs avoid equating unrelated ordinal intervals
  // across cameras. Four pairs retain the original <=9-image bound.
  bool capture_pairs = false;
  // Default-off exact-match arming. Nonzero selects the first qualifying draw
  // on/after this swap instead of guessing a timed/manual capture frame. The
  // state still locks ONE frame and the same four-pair budget. No guest input.
  uint64_t automatic_first_frame = 0;
};

inline bool IsValid(const Config& config) {
  if (!config.count) return true;
  return config.surface && config.scissor_br && config.first &&
         config.stride && config.count <= 8 &&
         (!config.capture_pairs || (config.pixel_shader_hash && config.count <= 4)) &&
         (!config.automatic_first_frame || config.capture_pairs) &&
         uint64_t(config.first) + uint64_t(config.count - 1) * config.stride <= 8192;
}

struct Draw {
  uint64_t frame = 0;
  uint64_t submitted_draw = 0;
  bool selected_frame = false;
  bool pixel_shader = false;
  uint32_t surface = 0;
  uint32_t color = 0;
  uint32_t color_mask = 0;
  uint32_t window_offset = 0;
  uint32_t scissor_tl = 0;
  uint32_t scissor_br = 0;
  uint64_t pixel_shader_hash = 0;
};

struct Selection {
  uint32_t writer = 0;
  bool before = false;
  bool after = false;
};

class State {
 public:
  uint64_t locked_frame() const { return frame_; }

  Selection Observe(const Config& config, const Draw& draw) {
    if (finished_ || !config.count || !IsValid(config)) return {};
    // Close even if the first draw of the next frame does not match the target.
    if (frame_ && draw.frame != frame_) {
      finished_ = true;
      return {};
    }
    const bool eligible_frame = config.automatic_first_frame
        ? draw.frame >= config.automatic_first_frame : draw.selected_frame;
    if (!draw.frame || !eligible_frame || !draw.pixel_shader ||
        draw.surface != config.surface || draw.color != config.color ||
        !(draw.color_mask & 0xF) || (draw.color_mask & ~0xFu) ||
        draw.window_offset || draw.scissor_tl ||
        draw.scissor_br != config.scissor_br ||
        (config.pixel_shader_hash && draw.pixel_shader_hash != config.pixel_shader_hash) ||
        draw.submitted_draw < config.minimum_draw) return {};
    if (!frame_) frame_ = draw.frame;
    // Observe exactly once per actually submitted draw, never again on retry.
    if (draw.submitted_draw <= last_draw_) return {};
    last_draw_ = draw.submitted_draw;
    Selection selection{++writers_, writers_ == 1, false};
    if (writers_ >= config.first &&
        (writers_ - config.first) % config.stride == 0 &&
        (writers_ - config.first) / config.stride < config.count) {
      selection.after = true;
    }
    if (writers_ >= uint64_t(config.first) +
                        uint64_t(config.count - 1) * config.stride) {
      finished_ = true;
    }
    if (config.capture_pairs) selection.before = selection.after;
    return selection;
  }

 private:
  uint64_t frame_ = 0;
  uint64_t last_draw_ = 0;
  uint32_t writers_ = 0;
  bool finished_ = false;
};

}  // namespace rex::graphics::embedded_target_writer_capture_policy
