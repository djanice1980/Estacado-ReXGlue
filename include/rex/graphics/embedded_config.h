#pragma once

#include <rex/config_contents.h>
#include <rex/cvar.h>

#include <filesystem>
#include <string_view>

namespace rex::graphics {
// Shared by embedded creation and the production-helper regression. Absence is
// intentional: the packaged example used for cache identity is not GPU config.
inline bool LoadEmbeddedPcConfig(bool present, std::string_view contents,
                                 const std::filesystem::path& origin) {
  if (present && !rex::cvar::LoadConfigContents(contents, origin)) return false;
  rex::cvar::ApplyEnvironment();
  return true;
}

inline std::filesystem::path ResolveEmbeddedShaderReplacementPackPath(
    const std::filesystem::path& configured_path,
    const char* config_path_utf8, const char* asset_root_utf8) {
  if (configured_path.is_absolute()) return configured_path.lexically_normal();
  const std::filesystem::path config_directory =
      config_path_utf8 && config_path_utf8[0]
          ? std::filesystem::u8path(config_path_utf8).parent_path()
          : std::filesystem::current_path();
  auto resolved = std::filesystem::absolute(config_directory / configured_path)
                      .lexically_normal();
  // An existing custom resource beside the original configuration wins. The
  // portable package is the fallback even when the process directory differs.
  if (!std::filesystem::is_regular_file(resolved)) {
    const auto asset_root = asset_root_utf8 && asset_root_utf8[0]
        ? std::filesystem::u8path(asset_root_utf8) : std::filesystem::path();
    const auto packaged = std::filesystem::absolute(asset_root / configured_path)
                              .lexically_normal();
    if (std::filesystem::is_regular_file(packaged)) resolved = packaged;
  }
  return resolved;
}
}
