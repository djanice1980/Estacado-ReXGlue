#pragma once

// HD texture packs, phase 1: texture identity and dumping
// (docs/NEXT_FEATURES_PLAN.md section 2 of The Darkness Recompiled).
//
// A guest texture's identity is an XXH3-64 hash of its description (format,
// size, mips, tiling, pitch, endianness) and of its guest data as stored (base
// level and mips, tiled or linear, big-endian). It does not include where the
// title placed the texture, so the same texture has the same id at any address
// and in every run - the name a pack file carries.
//
// Dumping (gpu_texture_dump, off by default): the texture cache copies each
// newly seen texture's host resource (what the title's shaders sample, after
// the loader's conversion) to a readback buffer after it is loaded, and once
// the GPU has finished, a DDS file (DX10 header, the host format) is written
// by a background thread - the command processor only hashes and records one
// copy. With the option off nothing here runs.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rex::graphics::texture_pack {

// Everything that identifies a guest texture apart from its address.
struct GuestTextureDesc {
  uint32_t format = 0;     // xenos::TextureFormat
  uint32_t dimension = 0;  // xenos::DataDimension
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth_or_array_size = 0;
  uint32_t mip_max_level = 0;
  uint32_t pitch = 0;  // texels / 32
  uint32_t tiled = 0;
  uint32_t packed_mips = 0;
  uint32_t endianness = 0;
  uint32_t signed_separate = 0;
};

uint64_t ContentId(const GuestTextureDesc& desc, const uint8_t* base, size_t base_size,
                   const uint8_t* mips, size_t mips_size);

// 16 lowercase hex digits (the pack file name without extension).
std::string IdName(uint64_t id);
// UTF-8 text of a path for logs (never throws on names outside the code page).
std::string PathText(const std::filesystem::path& path);

// Where packs are read from: gpu_texture_pack_root, else texture_packs next to
// the game executable (not the working directory).
std::filesystem::path PackFolder();
// Where gpu_texture_dump writes: gpu_texture_dump_root, else texture_dump
// next to the game executable.
std::filesystem::path DumpFolder();
// For players: "the texture_packs folder in the game folder".
std::string PackFolderHint();
// Textures of the active language pack (gpu_texture_language_root, set by the
// host while a pack is active; empty otherwise). Its replacements apply
// whether or not HD texture packs are on, and win over them.
std::filesystem::path LanguageFolder();

// DXGI formats that DDS readers accept for a file header (typeless -> UNORM).
uint32_t DdsFileFormat(uint32_t dxgi_format);

// A DDS file with a DX10 header for a 2D texture (array size 1); data holds
// the mip levels (mip 0 first), each tightly packed (no row padding: rows of
// ceil(width / block width) blocks, ceil(height / block height) rows).
std::vector<uint8_t> BuildDds2D(uint32_t dxgi_format, uint32_t width, uint32_t height,
                                uint32_t mip_levels, const std::vector<uint8_t>& data);

// Writes files on a background thread in order; Enqueue returns false (and
// drops the file) when more than kMaxQueuedBytes are waiting.
class FileWriter {
 public:
  static constexpr size_t kMaxQueuedBytes = size_t(512) << 20;
  static FileWriter& Get();
  bool Enqueue(std::filesystem::path path, std::vector<uint8_t> bytes);
  uint64_t written() const;
  uint64_t dropped() const;

 private:
  FileWriter() = default;
  struct Impl;
  Impl* impl();
};

// ---- Phase 2: replacement packs ----------------------------------------
// Pack files are <root>/replace/**/<id>.dds (16 hex digits, any subfolder):
// 2D, array size 1, any size and mip count, DX10 header or the legacy DXT1/3/5,
// ATI1/ATI2 (BC4/BC5) and 32-bit RGBA/BGRA headers. The replacement keeps the
// channel layout of the dump it was made from (the title's swizzle applies).

struct FormatBlock {
  uint32_t width = 0;  // texels per block (1 for uncompressed)
  uint32_t height = 0;
  uint32_t bytes = 0;  // bytes per block (0 = unsupported)
};
FormatBlock GetFormatBlock(uint32_t dxgi_format);

struct DdsImage {
  uint32_t dxgi_format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_levels = 0;
  // Tightly packed mips, mip 0 first.
  std::vector<uint8_t> data;
  std::vector<size_t> mip_offsets;
  std::vector<uint32_t> mip_row_bytes;
  std::vector<uint32_t> mip_rows;
};
// Header only (bytes may be just the first 148 bytes of the file) when
// with_data is false: dimensions, format and mips, for the VRAM estimate.
bool ParseDds(const uint8_t* bytes, size_t size, bool with_data, DdsImage& out,
              std::string& error);
// A whole .dds file read and parsed (with its pixels).
bool ReadDdsFile(const std::filesystem::path& path, DdsImage& out, std::string& error);
// GPU bytes of a parsed image's mip chain (tight; the driver may pad).
uint64_t DdsGpuBytes(const DdsImage& image);

struct PackEntry {
  std::filesystem::path path;
  uint64_t file_bytes = 0;
  uint64_t gpu_bytes = 0;  // 0 when the header could not be read
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dxgi_format = 0;
  // The replaced title texture (guest width, height and format), from the
  // texture dump's <id>.json beside the file when present.
  bool guest_known = false;
  uint64_t guest_key = 0;
  // A language pack's glyph overlay (<id>.json "overlay": {"x": .., "y": ..}):
  // blocks laid over the title's own texture at (x, y) instead of replacing
  // it. The texture cache reads the loaded texture back once and uploads the
  // combination, so the pack carries none of the title's pixels.
  bool overlay = false;
  uint32_t overlay_x = 0;
  uint32_t overlay_y = 0;
};
struct PackIndex {
  std::unordered_map<uint64_t, PackEntry> entries;
  uint64_t file_bytes = 0;
  uint64_t gpu_bytes = 0;
  uint32_t rejected = 0;    // unreadable or unsupported headers
  uint32_t duplicates = 0;  // the same id twice (the first file found stays)
  // When every entry names its title texture: only textures of these guest
  // sizes and formats need a content id (FinalizePackIndex).
  bool guest_filter = false;
  std::unordered_set<uint64_t> guest_keys;
};
// Guest width, height and format of a title texture as one key.
inline uint64_t GuestTextureKey(uint32_t width, uint32_t height, uint32_t format) {
  return (uint64_t(width & 0xFFFFFu) << 40) | (uint64_t(height & 0xFFFFFu) << 20) |
         uint64_t(format & 0xFFFFFu);
}
// Scans the folder and all its subfolders (headers only): every
// <16 hex digits>.dds file is a replacement, so packs can sit side by side.
PackIndex BuildPackIndex(const std::filesystem::path& folder);
// Adds the entries of from whose ids into does not have yet (counted as
// duplicates otherwise).
void MergePackIndex(PackIndex& into, PackIndex&& from);
// Sets guest_filter and guest_keys once the index is complete.
void FinalizePackIndex(PackIndex& index);

// Loads and parses pack files on a background thread; the command processor
// polls finished images without blocking.
class ReplacementLoader {
 public:
  static ReplacementLoader& Get();
  // Backend work for a parsed image, run on the loader thread (for example
  // creating the GPU texture and filling its upload buffer), so the command
  // processor only records copies. Its payload comes back with the image.
  using Prepare = std::function<std::shared_ptr<void>(const DdsImage& image)>;
  void SetPrepare(Prepare prepare);
  void Request(uint64_t id, std::filesystem::path path);
  // An image built in memory (an overlay laid over the title's texture),
  // prepared on the loader thread like a loaded file.
  void Offer(uint64_t id, std::shared_ptr<DdsImage> image);
  struct Finished {
    uint64_t id = 0;
    std::shared_ptr<DdsImage> image;  // mip_levels == 0 when loading failed
    std::shared_ptr<void> payload;    // from Prepare (null when it failed)
  };
  // Moves out everything finished since the last call.
  std::vector<Finished> TakeFinished();

 private:
  ReplacementLoader() = default;
  struct Impl;
  Impl* impl();
};

}  // namespace rex::graphics::texture_pack
