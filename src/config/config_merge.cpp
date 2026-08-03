#include "config/config_merge.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <set>
#include <string_view>
#include <system_error>

namespace umbriel::configmerge {

  namespace {

    constexpr Logger kLog("config");

    std::filesystem::path canonicalKey(const std::filesystem::path& path) {
      std::error_code error;
      auto key = std::filesystem::weakly_canonical(path, error);
      return error ? path.lexically_normal() : key;
    }

    std::string expandEnvironment(std::string_view input) {
      std::string result;
      result.reserve(input.size());
      const auto isNameStart = [](char c) {
        const auto value = static_cast<unsigned char>(c);
        return std::isalpha(value) != 0 || c == '_';
      };
      const auto isNameCharacter = [](char c) {
        const auto value = static_cast<unsigned char>(c);
        return std::isalnum(value) != 0 || c == '_';
      };

      for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '$') {
          result.push_back(input[i++]);
          continue;
        }
        if (i + 1 < input.size() && input[i + 1] == '{') {
          const std::size_t close = input.find('}', i + 2);
          if (close != std::string_view::npos) {
            const std::string name(input.substr(i + 2, close - i - 2));
            if (const char* value = std::getenv(name.c_str()); value != nullptr) {
              result.append(value);
            }
            i = close + 1;
            continue;
          }
        } else if (i + 1 < input.size() && isNameStart(input[i + 1])) {
          std::size_t end = i + 2;
          while (end < input.size() && isNameCharacter(input[end])) {
            ++end;
          }
          const std::string name(input.substr(i + 1, end - i - 1));
          if (const char* value = std::getenv(name.c_str()); value != nullptr) {
            result.append(value);
          }
          i = end;
          continue;
        }
        result.push_back(input[i++]);
      }
      return result;
    }

    std::filesystem::path expandPath(std::string_view raw, const std::filesystem::path& baseDir) {
      std::string expanded = expandEnvironment(raw);
      std::filesystem::path path;
      if (expanded == "~") {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
          path = home;
        } else {
          path = expanded;
        }
      } else if (expanded.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
          path = std::filesystem::path(home) / expanded.substr(2);
        } else {
          path = expanded;
        }
      } else {
        path = expanded;
      }
      if (!path.is_absolute()) {
        path = baseDir / path;
      }
      return path.lexically_normal();
    }

    struct IncludeDirective {
      std::vector<std::string> files;
    };

    IncludeDirective readInclude(const toml::table& table) {
      IncludeDirective directive;
      const toml::node* node = table.get("include");
      if (node == nullptr) {
        return directive;
      }
      const auto* include = node->as_table();
      if (include == nullptr) {
        kLog.warn("config: ignoring include (expected table)");
        return directive;
      }
      const toml::node* filesNode = include->get("files");
      if (filesNode == nullptr) {
        return directive;
      }
      const auto* files = filesNode->as_array();
      if (files == nullptr) {
        kLog.warn("config: ignoring include.files (expected array of strings)");
        return directive;
      }
      for (const auto& entry : *files) {
        if (!entry.is_string()) {
          kLog.warn("config: ignoring include.files (expected array of strings)");
          directive.files.clear();
          return directive;
        }
        directive.files.push_back(*entry.value<std::string>());
      }
      return directive;
    }

    toml::table
    loadAndExpand(const std::filesystem::path& path, std::set<std::filesystem::path>& visited, MergeResult& result);

    toml::table expandFile(
        const std::filesystem::path& path, const toml::table& parsed, std::set<std::filesystem::path>& visited,
        MergeResult& result
    ) {
      const auto key = canonicalKey(path);
      if (visited.contains(key)) {
        kLog.warn("config include cycle or duplicate skipped: {}", key.string());
        return {};
      }
      visited.insert(key);
      result.loadedFiles.push_back(key);

      toml::table base;
      const IncludeDirective directive = readInclude(parsed);
      for (const auto& entry : directive.files) {
        const auto target = expandPath(entry, path.parent_path());
        std::error_code error;
        if (std::filesystem::is_regular_file(target, error) && !error) {
          deepMerge(base, loadAndExpand(target, visited, result));
          continue;
        }
        if (result.firstError.empty()) {
          result.firstError = std::format("include not found: {} (from {})", entry, path.string());
        }
        kLog.warn("config include not found: {} (from {})", target.string(), path.string());
        result.loadedFiles.push_back(canonicalKey(target));
      }

      toml::table body = parsed;
      body.erase("include");
      deepMerge(base, body);
      return base;
    }

    toml::table
    loadAndExpand(const std::filesystem::path& path, std::set<std::filesystem::path>& visited, MergeResult& result) {
      toml::table parsed;
      try {
        parsed = toml::parse_file(path.string());
      } catch (const toml::parse_error& error) {
        result.hadParseError = true;
        const auto key = canonicalKey(path);
        if (std::ranges::find(result.loadedFiles, key) == result.loadedFiles.end()) {
          result.loadedFiles.push_back(key);
        }
        const auto& source = error.source();
        const std::string message = std::format(
            "{} line {}, column {}: {}", path.string(), source.begin.line, source.begin.column, error.description()
        );
        if (result.firstError.empty()) {
          result.firstError = message;
        }
        kLog.error("config parse error: {}", message);
        return {};
      }
      return expandFile(path, parsed, visited, result);
    }

  } // namespace

  void deepMerge(toml::table& base, const toml::table& overlay) {
    for (const auto& [key, value] : overlay) {
      if (const auto* overlayTable = value.as_table()) {
        if (auto* baseNode = base.get(key)) {
          if (auto* baseTable = baseNode->as_table()) {
            deepMerge(*baseTable, *overlayTable);
            continue;
          }
        }
      }
      base.insert_or_assign(key, value);
    }
  }

  MergeResult mergeWithIncludes(const std::filesystem::path& rootFile) {
    MergeResult result;
    std::set<std::filesystem::path> visited;
    result.merged = loadAndExpand(rootFile, visited, result);
    return result;
  }

} // namespace umbriel::configmerge
