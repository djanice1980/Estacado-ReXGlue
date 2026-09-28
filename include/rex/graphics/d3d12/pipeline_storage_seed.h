/**
 * @file graphics/d3d12/pipeline_storage_seed.h
 * @brief Imports a packaged shader/pipeline storage seed into the persistent
 *        D3D12 storage before PipelineCache loads it.
 *
 * The persistent storage ("shaders/shareable") holds only guest shader
 * microcode, content-addressed by XXH3, and pipeline descriptions, each
 * guarded by an XXH3 of its bytes. At load every shader is retranslated and
 * every pipeline recreated by the current code, so the records carry no
 * build-specific artifacts; format changes are rejected by the file headers
 * (magic, API and version). A seed record is appended only if it validates
 * exactly as the loader validates it and the store does not already hold it.
 * Nothing is removed from a store: a store whose header no longer matches is
 * replaced, which is what the loader itself does with such a file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <rex/hash.h>

namespace rex::graphics::d3d12::pipeline_storage_seed {

// On-disk constants mirrored from PipelineCache; pipeline_cache.cpp
// static_asserts that the sizes and versions match its private definitions.
inline constexpr uint32_t kShaderMagic = 0x48534558;     // 'XESH'
inline constexpr uint32_t kPipelineMagic = 0x53504558;   // 'XEPS'
inline constexpr uint32_t kPipelineApiRtv = 0x54525844;  // 'DXRT'
inline constexpr uint32_t kPipelineApiRov = 0x4F525844;  // 'DXRO'
inline constexpr size_t kShaderRecordHeaderSize = 12;
inline constexpr size_t kPipelineRecordSize = 72;

inline constexpr uint32_t ByteSwap32(uint32_t value) {
  return (value >> 24) | ((value >> 8) & 0x0000FF00u) |
         ((value << 8) & 0x00FF0000u) | (value << 24);
}

struct Record {
  size_t offset = 0;
  size_t size = 0;
  uint64_t hash = 0;
};

inline void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
  uint8_t bytes[4];
  std::memcpy(bytes, &value, sizeof(bytes));
  out.insert(out.end(), bytes, bytes + sizeof(bytes));
}

inline std::vector<uint8_t> ShaderHeader(uint32_t version) {
  std::vector<uint8_t> header;
  AppendU32(header, kShaderMagic);
  AppendU32(header, ByteSwap32(version));
  return header;
}

inline std::vector<uint8_t> PipelineHeader(uint32_t api, uint32_t version) {
  std::vector<uint8_t> header;
  AppendU32(header, kPipelineMagic);
  AppendU32(header, api);
  AppendU32(header, ByteSwap32(version));
  return header;
}

// Returns the byte length of the valid prefix (header plus every record up to
// the first one the loader would reject), or 0 if the header does not match.
inline size_t ParseShaders(const std::vector<uint8_t>& data, uint32_t version,
                           std::vector<Record>& records) {
  records.clear();
  const std::vector<uint8_t> header = ShaderHeader(version);
  if (data.size() < header.size() ||
      std::memcmp(data.data(), header.data(), header.size())) {
    return 0;
  }
  size_t offset = header.size();
  while (data.size() - offset >= kShaderRecordHeaderSize) {
    uint64_t hash;
    uint32_t bits;
    std::memcpy(&hash, data.data() + offset, sizeof(hash));
    std::memcpy(&bits, data.data() + offset + sizeof(hash), sizeof(bits));
    const size_t ucode_bytes = size_t(bits & 0x7FFFFFFFu) * sizeof(uint32_t);
    if (data.size() - offset - kShaderRecordHeaderSize < ucode_bytes ||
        XXH3_64bits(data.data() + offset + kShaderRecordHeaderSize,
                    ucode_bytes) != hash) {
      break;
    }
    records.push_back({offset, kShaderRecordHeaderSize + ucode_bytes, hash});
    offset += kShaderRecordHeaderSize + ucode_bytes;
  }
  return offset;
}

inline size_t ParsePipelines(const std::vector<uint8_t>& data, uint32_t api,
                             uint32_t version, std::vector<Record>& records) {
  records.clear();
  const std::vector<uint8_t> header = PipelineHeader(api, version);
  if (data.size() < header.size() ||
      std::memcmp(data.data(), header.data(), header.size())) {
    return 0;
  }
  size_t offset = header.size();
  while (data.size() - offset >= kPipelineRecordSize) {
    uint64_t hash;
    std::memcpy(&hash, data.data() + offset, sizeof(hash));
    if (XXH3_64bits(data.data() + offset + sizeof(hash),
                    kPipelineRecordSize - sizeof(hash)) != hash) {
      break;
    }
    records.push_back({offset, kPipelineRecordSize, hash});
    offset += kPipelineRecordSize;
  }
  return offset;
}

inline bool ReadWholeFile(const std::filesystem::path& path,
                          std::vector<uint8_t>& out) {
  out.clear();
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return false;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  out.assign(std::istreambuf_iterator<char>(stream),
             std::istreambuf_iterator<char>());
  return !stream.bad();
}

// Replaces the file through a sibling temporary so an interrupted write never
// leaves a partially written store behind.
inline bool WriteWholeFile(const std::filesystem::path& path,
                           const std::vector<uint8_t>& data) {
  std::filesystem::path temporary = path;
  temporary += ".seed-tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(data.data()),
                 std::streamsize(data.size()));
    stream.flush();
    if (!stream) return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

struct FileResult {
  bool seed_valid = false;    // seed present with a matching header
  size_t seed_records = 0;    // valid records in the seed
  size_t store_records = 0;   // valid records already in the store
  size_t added = 0;           // seed records appended to the store
  bool write_failed = false;
};

// Shaders are keyed by their microcode hash (the loader's own key); pipeline
// descriptions by their full bytes.
template <typename Parse>
FileResult MergeFile(const std::filesystem::path& seed_path,
                     const std::filesystem::path& store_path,
                     const std::vector<uint8_t>& header, Parse parse,
                     bool key_by_hash) {
  FileResult result;
  std::vector<uint8_t> seed;
  std::vector<Record> seed_records;
  if (!ReadWholeFile(seed_path, seed) || !parse(seed, seed_records)) {
    return result;
  }
  result.seed_valid = true;
  result.seed_records = seed_records.size();

  std::vector<uint8_t> store;
  std::vector<Record> store_records;
  const size_t store_valid =
      ReadWholeFile(store_path, store) ? parse(store, store_records) : 0;
  result.store_records = store_records.size();

  const auto key_of = [key_by_hash](const std::vector<uint8_t>& data,
                                    const Record& record) {
    if (key_by_hash) {
      return std::string(reinterpret_cast<const char*>(&record.hash),
                         sizeof(record.hash));
    }
    return std::string(reinterpret_cast<const char*>(data.data() + record.offset),
                       record.size);
  };
  std::unordered_set<std::string> present;
  for (const Record& record : store_records) present.insert(key_of(store, record));

  std::vector<uint8_t> merged;
  if (store_valid) {
    merged.assign(store.begin(), store.begin() + ptrdiff_t(store_valid));
  } else {
    merged = header;
  }
  for (const Record& record : seed_records) {
    if (!present.insert(key_of(seed, record)).second) continue;
    merged.insert(merged.end(), seed.begin() + ptrdiff_t(record.offset),
                  seed.begin() + ptrdiff_t(record.offset + record.size));
    ++result.added;
  }
  if (result.added && !WriteWholeFile(store_path, merged)) {
    result.write_failed = true;
    result.added = 0;
  }
  return result;
}

inline FileResult MergeShaders(const std::filesystem::path& seed_path,
                               const std::filesystem::path& store_path,
                               uint32_t version) {
  return MergeFile(
      seed_path, store_path, ShaderHeader(version),
      [version](const std::vector<uint8_t>& data, std::vector<Record>& records) {
        return ParseShaders(data, version, records);
      },
      true);
}

inline FileResult MergePipelines(const std::filesystem::path& seed_path,
                                 const std::filesystem::path& store_path,
                                 uint32_t api, uint32_t version) {
  return MergeFile(
      seed_path, store_path, PipelineHeader(api, version),
      [api, version](const std::vector<uint8_t>& data,
                     std::vector<Record>& records) {
        return ParsePipelines(data, api, version, records);
      },
      false);
}

}  // namespace rex::graphics::d3d12::pipeline_storage_seed
