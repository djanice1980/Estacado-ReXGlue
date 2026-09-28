/**
 ******************************************************************************
 * @file        graphics/pipeline/shader/replacement_pack.cpp
 * @brief       Data-driven guest shader replacement packs
 ******************************************************************************
 */

#include <rex/graphics/pipeline/shader/replacement_pack.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include <xxhash.h>

namespace rex::graphics {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {'R', 'E', 'X', 'S', 'R', 'P', '1', 0};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaximumRecords = 4096;
constexpr uint32_t kMaximumUcodeBytes = 1u << 20;
constexpr uint64_t kMaximumPackBytes = uint64_t(1) << 30;

bool Fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

bool ReadU32(std::span<const uint8_t> bytes, size_t& offset, uint32_t& value) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  offset += sizeof(value);
  return true;
}

bool ReadU64(std::span<const uint8_t> bytes, size_t& offset, uint64_t& value) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  offset += sizeof(value);
  return true;
}

ShaderReplacementPack& ConfiguredPack() {
  static ShaderReplacementPack pack;
  return pack;
}

struct PatchRules {
  std::unordered_map<uint64_t, ShaderPatch> vertex;
  std::unordered_map<uint64_t, ShaderPatch> pixel;
  // Replacements built from the rules (nullptr: the rule declined).
  std::mutex mutex;
  std::unordered_map<uint64_t, std::unique_ptr<ShaderReplacement>> built;
};

PatchRules& ConfiguredPatches() {
  static PatchRules rules;
  return rules;
}

}  // namespace

bool ShaderReplacementPack::Load(const std::filesystem::path& path,
                                 std::string* error) {
  Clear();
  if (error) {
    error->clear();
  }

  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return Fail(error, "unable to open shader replacement pack");
  }
  const std::streamoff stream_size = stream.tellg();
  if (stream_size < 0 || uint64_t(stream_size) > kMaximumPackBytes) {
    return Fail(error, "shader replacement pack size is invalid");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(stream_size));
  stream.seekg(0);
  if (!bytes.empty() &&
      !stream.read(reinterpret_cast<char*>(bytes.data()), stream_size)) {
    return Fail(error, "unable to read shader replacement pack");
  }
  const std::span<const uint8_t> data(bytes);
  if (data.size() < kMagic.size() + 8 ||
      std::memcmp(data.data(), kMagic.data(), kMagic.size()) != 0) {
    return Fail(error, "shader replacement pack magic is invalid");
  }

  size_t offset = kMagic.size();
  uint32_t version = 0;
  uint32_t record_count = 0;
  if (!ReadU32(data, offset, version) || !ReadU32(data, offset, record_count) ||
      version != kVersion || record_count > kMaximumRecords) {
    return Fail(error, "shader replacement pack header is invalid");
  }

  std::unordered_map<uint64_t, ShaderReplacement> vertex;
  std::unordered_map<uint64_t, ShaderReplacement> pixel;
  for (uint32_t record_index = 0; record_index < record_count;
       ++record_index) {
    uint64_t source_hash = 0;
    uint64_t replacement_hash = 0;
    uint32_t stage_value = 0;
    uint32_t ucode_bytes = 0;
    if (!ReadU64(data, offset, source_hash) ||
        !ReadU64(data, offset, replacement_hash) ||
        !ReadU32(data, offset, stage_value) ||
        !ReadU32(data, offset, ucode_bytes)) {
      return Fail(error, "shader replacement record header is truncated");
    }
    if (stage_value > uint32_t(xenos::ShaderType::kPixel) ||
        !source_hash || !replacement_hash || source_hash == replacement_hash ||
        !ucode_bytes || (ucode_bytes & 3u) != 0 ||
        ucode_bytes > kMaximumUcodeBytes || offset > data.size() ||
        data.size() - offset < ucode_bytes) {
      return Fail(error, "shader replacement record is invalid");
    }
    if (XXH3_64bits(data.data() + offset, ucode_bytes) != replacement_hash) {
      return Fail(error, "shader replacement microcode hash does not match");
    }

    ShaderReplacement replacement;
    replacement.stage = xenos::ShaderType(stage_value);
    replacement.source_hash = source_hash;
    replacement.replacement_hash = replacement_hash;
    replacement.ucode.resize(ucode_bytes / sizeof(uint32_t));
    std::memcpy(replacement.ucode.data(), data.data() + offset, ucode_bytes);
    offset += ucode_bytes;

    auto& target = replacement.stage == xenos::ShaderType::kVertex
                       ? vertex
                       : pixel;
    if (!target.emplace(source_hash, std::move(replacement)).second) {
      return Fail(error, "shader replacement source hash is duplicated");
    }
  }
  if (offset != data.size()) {
    return Fail(error, "shader replacement pack has trailing data");
  }

  vertex_ = std::move(vertex);
  pixel_ = std::move(pixel);
  return true;
}

void ShaderReplacementPack::Clear() {
  vertex_.clear();
  pixel_.clear();
}

const ShaderReplacement* ShaderReplacementPack::Find(
    xenos::ShaderType stage, uint64_t source_hash) const {
  const auto& target = stage == xenos::ShaderType::kVertex ? vertex_ : pixel_;
  const auto iterator = target.find(source_hash);
  return iterator == target.end() ? nullptr : &iterator->second;
}

size_t ShaderReplacementPack::size() const {
  return vertex_.size() + pixel_.size();
}

bool ConfigureShaderReplacementPack(const std::filesystem::path& path,
                                    std::string* error) {
  return ConfiguredPack().Load(path, error);
}

const ShaderReplacement* FindConfiguredShaderReplacement(
    xenos::ShaderType stage, uint64_t source_hash) {
  return ConfiguredPack().Find(stage, source_hash);
}

size_t ConfiguredShaderReplacementCount() {
  return ConfiguredPack().size();
}

void ConfigureShaderPatch(xenos::ShaderType stage, uint64_t source_hash,
                          ShaderPatch patch) {
  PatchRules& rules = ConfiguredPatches();
  (stage == xenos::ShaderType::kVertex ? rules.vertex : rules.pixel)[source_hash] = patch;
}

size_t ConfiguredShaderPatchCount() {
  const PatchRules& rules = ConfiguredPatches();
  return rules.vertex.size() + rules.pixel.size();
}

const ShaderReplacement* FindConfiguredShaderReplacement(
    xenos::ShaderType stage, uint64_t source_hash, const uint32_t* ucode,
    uint32_t dword_count) {
  if (const ShaderReplacement* packed = ConfiguredPack().Find(stage, source_hash)) {
    return packed;
  }
  PatchRules& rules = ConfiguredPatches();
  const auto& table = stage == xenos::ShaderType::kVertex ? rules.vertex : rules.pixel;
  const auto rule = table.find(source_hash);
  if (rule == table.end() || !ucode || !dword_count) {
    return nullptr;
  }
  const uint64_t key = source_hash ^ (stage == xenos::ShaderType::kVertex ? 0 : 1);
  std::lock_guard<std::mutex> lock(rules.mutex);
  if (const auto done = rules.built.find(key); done != rules.built.end()) {
    return done->second.get();
  }
  auto replacement = std::make_unique<ShaderReplacement>();
  replacement->stage = stage;
  replacement->source_hash = source_hash;
  replacement->ucode.assign(ucode, ucode + dword_count);
  if (!rule->second(replacement->ucode)) {
    replacement.reset();
  } else {
    replacement->replacement_hash = XXH3_64bits(
        replacement->ucode.data(), replacement->ucode.size() * sizeof(uint32_t));
    if (replacement->replacement_hash == source_hash) replacement.reset();
  }
  // Once per shader: the result can be checked against a pack's hashes.
  std::fprintf(stderr, "REX_SHADER_PATCH stage=%s source=%016llX replacement=%016llX dwords=%u\n",
               stage == xenos::ShaderType::kVertex ? "vertex" : "pixel",
               static_cast<unsigned long long>(source_hash),
               static_cast<unsigned long long>(replacement ? replacement->replacement_hash : 0),
               dword_count);
  const ShaderReplacement* result = replacement.get();
  rules.built.emplace(key, std::move(replacement));
  return result;
}

}  // namespace rex::graphics
