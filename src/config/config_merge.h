#pragma once

#include "core/toml.h"
#include "config/config_diag.h"

#include <filesystem>
#include <string>
#include <vector>

namespace umbriel::configmerge {

  struct MergeResult {
    toml::table merged;
    std::vector<std::filesystem::path> loadedFiles;
    std::vector<ConfigDiagnostic> diagnostics;
    bool hadParseError = false;
  };

  [[nodiscard]] MergeResult mergeWithIncludes(const std::filesystem::path& rootFile);
  void deepMerge(toml::table& base, const toml::table& overlay);
  void deepMerge(toml::table& base, toml::table&& overlay);

} // namespace umbriel::configmerge
