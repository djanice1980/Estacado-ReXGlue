#pragma once

#include <filesystem>
#include <string_view>

namespace rex::cvar {
// Applies already-owned TOML bytes. The origin is diagnostic/resource identity,
// never a file to reopen. Returns false without applying a malformed document.
bool LoadConfigContents(std::string_view contents,
                        const std::filesystem::path& origin);
}
