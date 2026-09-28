// HD texture packs, phase 1: identity, DDS files and the background writer.
// See include/rex/graphics/pipeline/texture/texture_pack.h.

#include <rex/graphics/pipeline/texture/texture_pack.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/hash.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(gpu_texture_pack_root, "", "GPU",
                      "HD texture packs: folder scanned for <id>.dds files (any subfolders); "
                      "empty = texture_packs next to the game executable");
REXCVAR_DEFINE_STRING(gpu_texture_language_root, "", "GPU",
                      "Language pack textures: folder of <id>.dds replacements used while a "
                      "language pack is active (set by the host; independent of HD packs)");
REXCVAR_DEFINE_STRING(gpu_texture_dump_root, "", "GPU",
                      "HD texture packs (modding): dump folder; empty = texture_dump next to "
                      "the game executable");

namespace rex::graphics::texture_pack {

uint64_t ContentId(const GuestTextureDesc& desc, const uint8_t* base, size_t base_size,
                   const uint8_t* mips, size_t mips_size) {
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  // A version tag keeps ids of this scheme apart from any later one.
  static constexpr char kScheme[] = "rex-texture-id-1";
  XXH3_64bits_update(&state, kScheme, sizeof(kScheme) - 1);
  XXH3_64bits_update(&state, &desc, sizeof(desc));
  XXH3_64bits_update(&state, &base_size, sizeof(base_size));
  if (base && base_size) XXH3_64bits_update(&state, base, base_size);
  XXH3_64bits_update(&state, &mips_size, sizeof(mips_size));
  if (mips && mips_size) XXH3_64bits_update(&state, mips, mips_size);
  return XXH3_64bits_digest(&state);
}

std::string IdName(uint64_t id) {
  char name[17];
  std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(id));
  return name;
}

std::string PathText(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

namespace {
// The game executable (the working directory may differ).
const std::filesystem::path& GameExecutable() {
  static const std::filesystem::path executable = [] {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (length && length < buffer.size()) {
      return std::filesystem::path(std::wstring(buffer.data(), length));
    }
#endif
    return std::filesystem::path();
  }();
  return executable;
}
}  // namespace

std::filesystem::path PackFolder() {
  const std::string folder = REXCVAR_GET(gpu_texture_pack_root);
  if (!folder.empty()) return std::filesystem::path(folder);
  return GameExecutable().parent_path() / "texture_packs";
}

std::filesystem::path DumpFolder() {
  const std::string folder = REXCVAR_GET(gpu_texture_dump_root);
  if (!folder.empty()) return std::filesystem::path(folder);
  return GameExecutable().parent_path() / "texture_dump";
}

std::filesystem::path LanguageFolder() {
  const std::string folder = REXCVAR_GET(gpu_texture_language_root);
  return folder.empty() ? std::filesystem::path() : std::filesystem::u8path(folder);
}

std::string PackFolderHint() {
  if (!REXCVAR_GET(gpu_texture_pack_root).empty()) return PathText(PackFolder());
  return "the texture_packs folder in the game folder";
}

namespace {
// Wide names on Windows: the game folder may be named anything.
FILE* OpenPath(const std::filesystem::path& path, bool write) {
#if defined(_WIN32)
  return _wfopen(path.c_str(), write ? L"wb" : L"rb");
#else
  return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}
}  // namespace

uint32_t DdsFileFormat(uint32_t dxgi_format) {
  switch (dxgi_format) {
    case 27: return 28;   // R8G8B8A8_TYPELESS -> UNORM
    case 70: return 71;   // BC1_TYPELESS -> UNORM
    case 73: return 74;   // BC2_TYPELESS -> UNORM
    case 76: return 77;   // BC3_TYPELESS -> UNORM
    case 79: return 80;   // BC4_TYPELESS -> UNORM
    case 82: return 83;   // BC5_TYPELESS -> UNORM
    case 87: return 87;   // B8G8R8A8_UNORM
    case 90: return 87;   // B8G8R8A8_TYPELESS -> UNORM
    case 60: return 61;   // R8_TYPELESS -> UNORM
    case 48: return 49;   // R8G8_TYPELESS -> UNORM
    case 53: return 56;   // R16_TYPELESS -> UNORM
    case 33: return 35;   // R16G16_TYPELESS -> UNORM
    case 9: return 11;    // R16G16B16A16_TYPELESS -> UNORM
    case 23: return 24;   // R10G10B10A2_TYPELESS -> UNORM
    default: return dxgi_format;
  }
}

namespace {
void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}
}  // namespace

std::vector<uint8_t> BuildDds2D(uint32_t dxgi_format, uint32_t width, uint32_t height,
                                uint32_t mip_levels, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  out.reserve(4 + 124 + 20 + data.size());
  Put32(out, 0x20534444u);  // "DDS "
  // DDS_HEADER
  Put32(out, 124);
  // CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT
  Put32(out, 0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u);
  Put32(out, height);
  Put32(out, width);
  Put32(out, 0);  // pitch or linear size (optional)
  Put32(out, 0);  // depth
  Put32(out, mip_levels);
  for (int i = 0; i < 11; ++i) Put32(out, 0);
  // DDS_PIXELFORMAT: FOURCC "DX10"
  Put32(out, 32);
  Put32(out, 0x4u);
  Put32(out, 0x30315844u);
  for (int i = 0; i < 5; ++i) Put32(out, 0);
  // TEXTURE | MIPMAP | COMPLEX
  Put32(out, 0x1000u | (mip_levels > 1 ? 0x400000u | 0x8u : 0u));
  Put32(out, 0);
  Put32(out, 0);
  Put32(out, 0);
  Put32(out, 0);
  // DDS_HEADER_DXT10
  Put32(out, DdsFileFormat(dxgi_format));
  Put32(out, 3);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
  Put32(out, 0);
  Put32(out, 1);  // array size
  Put32(out, 0);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

struct FileWriter::Impl {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::pair<std::filesystem::path, std::vector<uint8_t>>> queue;
  size_t queued_bytes = 0;
  std::atomic<uint64_t> written{0};
  std::atomic<uint64_t> dropped{0};
  std::thread thread;

  Impl() {
    thread = std::thread([this] { Run(); });
    thread.detach();
  }

  void Run() {
    for (;;) {
      std::pair<std::filesystem::path, std::vector<uint8_t>> item;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [this] { return !queue.empty(); });
        item = std::move(queue.front());
        queue.pop_front();
        queued_bytes -= item.second.size();
      }
      std::error_code error;
      std::filesystem::create_directories(item.first.parent_path(), error);
      std::filesystem::path partial = item.first;
      partial += ".partial";
      if (FILE* file = OpenPath(partial, true)) {
        const bool ok =
            std::fwrite(item.second.data(), 1, item.second.size(), file) == item.second.size();
        std::fclose(file);
        if (ok) {
          std::filesystem::rename(partial, item.first, error);
          if (!error) {
            written.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
        }
        std::filesystem::remove(partial, error);
      }
      dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

FileWriter& FileWriter::Get() {
  static FileWriter writer;
  return writer;
}

FileWriter::Impl* FileWriter::impl() {
  // Created on first use only: with dumping off no thread exists.
  static Impl* instance = new Impl();
  return instance;
}

bool FileWriter::Enqueue(std::filesystem::path path, std::vector<uint8_t> bytes) {
  Impl* state = impl();
  {
    std::lock_guard lock(state->mutex);
    if (state->queued_bytes + bytes.size() > kMaxQueuedBytes) {
      state->dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    state->queued_bytes += bytes.size();
    state->queue.emplace_back(std::move(path), std::move(bytes));
  }
  state->wake.notify_one();
  return true;
}

uint64_t FileWriter::written() const {
  return const_cast<FileWriter*>(this)->impl()->written.load(std::memory_order_relaxed);
}

uint64_t FileWriter::dropped() const {
  return const_cast<FileWriter*>(this)->impl()->dropped.load(std::memory_order_relaxed);
}

// ---- Phase 2: replacement packs ------------------------------------------

FormatBlock GetFormatBlock(uint32_t dxgi_format) {
  switch (dxgi_format) {
    // BC1, BC4: 8 bytes per 4x4 block.
    case 70: case 71: case 72:   // BC1
    case 79: case 80: case 81:   // BC4
      return {4, 4, 8};
    // BC2, BC3, BC5, BC6H, BC7: 16 bytes per 4x4 block.
    case 73: case 74: case 75:   // BC2
    case 76: case 77: case 78:   // BC3
    case 82: case 83: case 84:   // BC5
    case 94: case 95: case 96:   // BC6H
    case 97: case 98: case 99:   // BC7
      return {4, 4, 16};
    case 27: case 28: case 29: case 30: case 31: case 32:  // R8G8B8A8
    case 87: case 88: case 90: case 91:                    // B8G8R8A8 / X8
    case 23: case 24: case 25:                             // R10G10B10A2
    case 33: case 34: case 35: case 36: case 37: case 38:  // R16G16
    case 26:                                               // R11G11B10
      return {1, 1, 4};
    case 9: case 10: case 11: case 12: case 13: case 14:   // R16G16B16A16
      return {1, 1, 8};
    case 48: case 49: case 50: case 51: case 52:           // R8G8
    case 53: case 54: case 55: case 56: case 57: case 58:  // R16
    case 85: case 86: case 115:                            // B5G6R5, B5G5R5A1, B4G4R4A4
      return {1, 1, 2};
    case 60: case 61: case 62: case 63: case 64: case 65:  // R8, A8
      return {1, 1, 1};
    default:
      return {};
  }
}

namespace {
uint32_t Get32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
         (uint32_t(bytes[3]) << 24);
}
constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
         (uint32_t(uint8_t(d)) << 24);
}
}  // namespace

bool ParseDds(const uint8_t* bytes, size_t size, bool with_data, DdsImage& out,
              std::string& error) {
  out = DdsImage();
  if (size < 128 || Get32(bytes) != 0x20534444u || Get32(bytes + 4) != 124) {
    error = "not a DDS file";
    return false;
  }
  const uint32_t flags = Get32(bytes + 8);
  out.height = Get32(bytes + 12);
  out.width = Get32(bytes + 16);
  const uint32_t depth = Get32(bytes + 24);
  out.mip_levels = (flags & 0x20000u) ? std::max(Get32(bytes + 28), 1u) : 1u;
  const uint8_t* pf = bytes + 76;
  const uint32_t pf_flags = Get32(pf + 4);
  const uint32_t fourcc = Get32(pf + 8);
  const uint32_t caps2 = Get32(bytes + 112);
  size_t data_offset = 128;
  if ((caps2 & 0x200u) || (depth > 1 && (flags & 0x800000u))) {
    error = "cube maps and volume textures are not supported";
    return false;
  }
  if ((pf_flags & 0x4u) && fourcc == FourCC('D', 'X', '1', '0')) {
    if (size < 148) {
      error = "truncated DX10 header";
      return false;
    }
    out.dxgi_format = Get32(bytes + 128);
    const uint32_t dimension = Get32(bytes + 132);
    const uint32_t misc = Get32(bytes + 136);
    const uint32_t array_size = Get32(bytes + 140);
    if (dimension != 3 || (misc & 0x4u) || array_size > 1) {
      error = "only 2D textures with one array slice are supported";
      return false;
    }
    data_offset = 148;
  } else if (pf_flags & 0x4u) {
    switch (fourcc) {
      case FourCC('D', 'X', 'T', '1'): out.dxgi_format = 71; break;
      case FourCC('D', 'X', 'T', '2'):
      case FourCC('D', 'X', 'T', '3'): out.dxgi_format = 74; break;
      case FourCC('D', 'X', 'T', '4'):
      case FourCC('D', 'X', 'T', '5'): out.dxgi_format = 77; break;
      case FourCC('A', 'T', 'I', '1'):
      case FourCC('B', 'C', '4', 'U'): out.dxgi_format = 80; break;
      case FourCC('A', 'T', 'I', '2'):
      case FourCC('B', 'C', '5', 'U'): out.dxgi_format = 83; break;
      default:
        error = "unsupported FourCC";
        return false;
    }
  } else if ((pf_flags & 0x40u) && Get32(pf + 12) == 32) {
    const uint32_t r_mask = Get32(pf + 16);
    if (r_mask == 0x000000FFu) {
      out.dxgi_format = 28;  // R8G8B8A8_UNORM
    } else if (r_mask == 0x00FF0000u) {
      out.dxgi_format = 87;  // B8G8R8A8_UNORM
    } else {
      error = "unsupported 32-bit channel masks";
      return false;
    }
  } else {
    error = "unsupported pixel format";
    return false;
  }
  switch (out.dxgi_format) {
    case 72: out.dxgi_format = 71; break;  // BC1_UNORM_SRGB
    case 75: out.dxgi_format = 74; break;  // BC2_UNORM_SRGB
    case 78: out.dxgi_format = 77; break;  // BC3_UNORM_SRGB
    case 99: out.dxgi_format = 98; break;  // BC7_UNORM_SRGB
    case 29: out.dxgi_format = 28; break;  // R8G8B8A8_UNORM_SRGB
    case 91: out.dxgi_format = 87; break;  // B8G8R8A8_UNORM_SRGB
    default: break;
  }
  const FormatBlock block = GetFormatBlock(out.dxgi_format);
  if (!block.bytes || !out.width || !out.height || out.width > 16384 || out.height > 16384 ||
      out.mip_levels > 15) {
    error = "unsupported format or size";
    return false;
  }
  size_t offset = 0;
  for (uint32_t level = 0; level < out.mip_levels; ++level) {
    const uint32_t width = std::max(out.width >> level, 1u);
    const uint32_t height = std::max(out.height >> level, 1u);
    const uint32_t row_bytes = (width + block.width - 1) / block.width * block.bytes;
    const uint32_t rows = (height + block.height - 1) / block.height;
    out.mip_offsets.push_back(offset);
    out.mip_row_bytes.push_back(row_bytes);
    out.mip_rows.push_back(rows);
    offset += size_t(row_bytes) * rows;
  }
  if (with_data) {
    if (size < data_offset + offset) {
      error = "truncated pixel data";
      return false;
    }
    out.data.assign(bytes + data_offset, bytes + data_offset + offset);
  }
  return true;
}

bool ReadDdsFile(const std::filesystem::path& path, DdsImage& out, std::string& error) {
  std::vector<uint8_t> bytes;
  if (FILE* file = OpenPath(path, false)) {
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length > 0) {
      bytes.resize(size_t(length));
      bytes.resize(std::fread(bytes.data(), 1, bytes.size(), file));
    }
    std::fclose(file);
  }
  if (bytes.empty()) {
    error = "unreadable file";
    return false;
  }
  return ParseDds(bytes.data(), bytes.size(), true, out, error);
}

uint64_t DdsGpuBytes(const DdsImage& image) {
  uint64_t total = 0;
  for (size_t level = 0; level < image.mip_row_bytes.size(); ++level) {
    total += uint64_t(image.mip_row_bytes[level]) * image.mip_rows[level];
  }
  return total;
}

namespace {
bool ParseIdName(const std::string& stem, uint64_t& id) {
  if (stem.size() != 16) return false;
  id = 0;
  for (char c : stem) {
    uint32_t digit;
    if (c >= '0' && c <= '9') digit = uint32_t(c - '0');
    else if (c >= 'a' && c <= 'f') digit = uint32_t(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = uint32_t(c - 'A' + 10);
    else return false;
    id = (id << 4) | digit;
  }
  return true;
}
}  // namespace

namespace {
// "name": <unsigned> in a small JSON text; false when absent.
bool JsonUnsigned(const std::string& text, const char* name, uint32_t& value) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t at = text.find(key);
  if (at == std::string::npos) return false;
  size_t i = at + key.size();
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
  uint64_t v = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    v = v * 10 + uint32_t(text[i] - '0');
    if (v > 0xFFFFFFFFu) return false;
  }
  value = uint32_t(v);
  return true;
}

// The dump's description of the title texture (gpu_texture_dump writes it as
// <id>.json): guest_width/guest_height (else width/height) and guest_format.
void ReadGuestDescription(const std::filesystem::path& json, PackEntry& entry) {
  std::string text;
  if (FILE* file = OpenPath(json, false)) {
    char buffer[1024];
    const size_t got = std::fread(buffer, 1, sizeof(buffer), file);
    std::fclose(file);
    text.assign(buffer, got);
  }
  if (text.empty()) return;
  uint32_t width = 0, height = 0, format = 0;
  if (!JsonUnsigned(text, "guest_width", width)) JsonUnsigned(text, "width", width);
  if (!JsonUnsigned(text, "guest_height", height)) JsonUnsigned(text, "height", height);
  if (!JsonUnsigned(text, "guest_format", format) || !width || !height) return;
  entry.guest_known = true;
  entry.guest_key = GuestTextureKey(width, height, format);
  // "overlay": {"x": .., "y": ..} (a language pack's glyph blocks).
  if (const size_t overlay = text.find("\"overlay\""); overlay != std::string::npos) {
    const std::string rest = text.substr(overlay);
    uint32_t x = 0, y = 0;
    if (JsonUnsigned(rest, "x", x) && JsonUnsigned(rest, "y", y)) {
      entry.overlay = true;
      entry.overlay_x = x;
      entry.overlay_y = y;
    }
  }
}
}  // namespace

PackIndex BuildPackIndex(const std::filesystem::path& folder) {
  PackIndex index;
  std::error_code error;
  const std::filesystem::path& directory = folder;
  if (!std::filesystem::is_directory(directory, error)) {
    return index;
  }
  for (std::filesystem::recursive_directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error)) continue;
    const std::filesystem::path& path = it->path();
    std::string extension = PathText(path.extension());
    for (char& c : extension) c = char(std::tolower(uint8_t(c)));
    uint64_t id;
    if (extension != ".dds" || !ParseIdName(PathText(path.stem()), id)) continue;
    if (index.entries.count(id)) {
      ++index.duplicates;
      std::fprintf(stderr, "REX_TEXTURE_PACK_DUPLICATE file=%s kept=%s\n",
                   PathText(path).c_str(), PathText(index.entries[id].path).c_str());
      continue;
    }
    PackEntry entry;
    entry.path = path;
    entry.file_bytes = uint64_t(it->file_size(error));
    uint8_t header[148] = {};
    size_t got = 0;
    if (FILE* file = OpenPath(path, false)) {
      got = std::fread(header, 1, sizeof(header), file);
      std::fclose(file);
    }
    DdsImage image;
    std::string parse_error;
    if (!ParseDds(header, got, false, image, parse_error)) {
      ++index.rejected;
      std::fprintf(stderr, "REX_TEXTURE_PACK_REJECTED file=%s reason=%s\n",
                   PathText(path).c_str(), parse_error.c_str());
      continue;
    }
    entry.gpu_bytes = DdsGpuBytes(image);
    entry.width = image.width;
    entry.height = image.height;
    entry.dxgi_format = image.dxgi_format;
    ReadGuestDescription(std::filesystem::path(path).replace_extension(".json"), entry);
    index.file_bytes += entry.file_bytes;
    index.gpu_bytes += entry.gpu_bytes;
    index.entries[id] = std::move(entry);
  }
  return index;
}

void MergePackIndex(PackIndex& into, PackIndex&& from) {
  for (auto& [id, entry] : from.entries) {
    if (into.entries.count(id)) {
      ++into.duplicates;
      continue;
    }
    into.file_bytes += entry.file_bytes;
    into.gpu_bytes += entry.gpu_bytes;
    into.entries.emplace(id, std::move(entry));
  }
  into.rejected += from.rejected;
  into.duplicates += from.duplicates;
}

void FinalizePackIndex(PackIndex& index) {
  index.guest_keys.clear();
  index.guest_filter = !index.entries.empty();
  for (const auto& [id, entry] : index.entries) {
    if (!entry.guest_known) {
      index.guest_filter = false;
      index.guest_keys.clear();
      return;
    }
    index.guest_keys.insert(entry.guest_key);
  }
}

struct ReplacementLoader::Impl {
  std::mutex mutex;
  std::condition_variable wake;
  struct Request {
    uint64_t id = 0;
    std::filesystem::path path;
    std::shared_ptr<DdsImage> image;  // built in memory (no file)
  };
  std::deque<Request> requests;
  std::vector<Finished> finished;
  Prepare prepare;
  std::thread thread;

  Impl() {
    thread = std::thread([this] { Run(); });
    thread.detach();
  }

  void Run() {
    for (;;) {
      Request request;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [this] { return !requests.empty(); });
        request = std::move(requests.front());
        requests.pop_front();
      }
      std::shared_ptr<DdsImage> image = request.image;
      std::string error;
      bool parsed = image != nullptr;
      if (!image) {
        image = std::make_shared<DdsImage>();
        parsed = ReadDdsFile(request.path, *image, error);
      }
      std::shared_ptr<void> payload;
      if (!parsed) {
        std::fprintf(stderr, "REX_TEXTURE_PACK_LOAD_FAILED file=%s reason=%s\n",
                     PathText(request.path).c_str(), error.c_str());
        image->mip_levels = 0;
      } else {
        Prepare prepare_copy;
        {
          std::lock_guard lock(mutex);
          prepare_copy = prepare;
        }
        if (prepare_copy) {
          payload = prepare_copy(*image);
          // The prepared upload holds the pixels now.
          std::vector<uint8_t>().swap(image->data);
        }
      }
      std::lock_guard lock(mutex);
      Finished result;
      result.id = request.id;
      result.image = std::move(image);
      result.payload = std::move(payload);
      finished.push_back(std::move(result));
    }
  }
};

ReplacementLoader& ReplacementLoader::Get() {
  static ReplacementLoader loader;
  return loader;
}

ReplacementLoader::Impl* ReplacementLoader::impl() {
  static Impl* instance = new Impl();
  return instance;
}

void ReplacementLoader::Request(uint64_t id, std::filesystem::path path) {
  Impl* state = impl();
  {
    std::lock_guard lock(state->mutex);
    state->requests.push_back({id, std::move(path), nullptr});
  }
  state->wake.notify_one();
}

void ReplacementLoader::Offer(uint64_t id, std::shared_ptr<DdsImage> image) {
  Impl* state = impl();
  {
    std::lock_guard lock(state->mutex);
    state->requests.push_back({id, {}, std::move(image)});
  }
  state->wake.notify_one();
}

void ReplacementLoader::SetPrepare(Prepare prepare) {
  Impl* state = impl();
  std::lock_guard lock(state->mutex);
  state->prepare = std::move(prepare);
}

std::vector<ReplacementLoader::Finished> ReplacementLoader::TakeFinished() {
  Impl* state = impl();
  std::vector<Finished> result;
  std::lock_guard lock(state->mutex);
  result.swap(state->finished);
  return result;
}

}  // namespace rex::graphics::texture_pack
