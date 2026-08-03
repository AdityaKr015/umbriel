#pragma once

#include "core/toml.h"

#include <filesystem>
#include <string>
#include <vector>

namespace umbriel::configmerge {

  struct MergeResult {
    toml::table merged;
    std::vector<std::filesystem::path> loadedFiles;
    std::string firstError;
    bool hadParseError = false;
  };

  [[nodiscard]] MergeResult mergeWithIncludes(const std::filesystem::path& rootFile);
  void deepMerge(toml::table& base, const toml::table& overlay);

} // namespace umbriel::configmerge
