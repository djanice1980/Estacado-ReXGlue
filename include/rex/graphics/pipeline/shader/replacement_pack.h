/**
 ******************************************************************************
 * @file        graphics/pipeline/shader/replacement_pack.h
 * @brief       Data-driven guest shader replacement packs
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

struct ShaderReplacement {
  xenos::ShaderType stage = xenos::ShaderType::kVertex;
  uint64_t source_hash = 0;
  uint64_t replacement_hash = 0;
  // Raw guest-endian microcode bytes retained in dword-aligned storage.
  std::vector<uint32_t> ucode;
};

// A pack contains only mappings and raw microcode supplied outside ReXGlue.
// This keeps title-specific shader data out of the runtime source while giving
// PC ports a reusable, validated replacement boundary.
class ShaderReplacementPack {
 public:
  bool Load(const std::filesystem::path& path, std::string* error = nullptr);
  void Clear();

  const ShaderReplacement* Find(xenos::ShaderType stage,
                                uint64_t source_hash) const;
  size_t size() const;

 private:
  std::unordered_map<uint64_t, ShaderReplacement> vertex_;
  std::unordered_map<uint64_t, ShaderReplacement> pixel_;
};

// Configure once during GPU plugin initialization, before shader loading or
// background pipeline work begins. The configured pack is immutable afterward.
bool ConfigureShaderReplacementPack(const std::filesystem::path& path,
                                    std::string* error = nullptr);
const ShaderReplacement* FindConfiguredShaderReplacement(
    xenos::ShaderType stage, uint64_t source_hash);
size_t ConfiguredShaderReplacementCount();

// Patch rules: replacements made while the title runs from its own microcode,
// so no title data has to be shipped. A rule edits the guest-endian microcode
// in place and returns true, or returns false to keep the original. Configure
// before shader loading, like the pack.
using ShaderPatch = bool (*)(std::vector<uint32_t>& ucode);
void ConfigureShaderPatch(xenos::ShaderType stage, uint64_t source_hash,
                          ShaderPatch patch);
size_t ConfiguredShaderPatchCount();

// The pack's replacement for a source hash, else the replacement a patch
// rule makes from this microcode (built once per source hash; thread-safe).
const ShaderReplacement* FindConfiguredShaderReplacement(
    xenos::ShaderType stage, uint64_t source_hash, const uint32_t* ucode,
    uint32_t dword_count);

}  // namespace rex::graphics
