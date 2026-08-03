#include "config/config.h"

#include "config/config_merge.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string_view>

namespace umbriel {

  namespace {

    constexpr Logger kLog("config");
    Config g_config;

    std::filesystem::path defaultConfigPath() {
      if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
          xdgConfigHome != nullptr && xdgConfigHome[0] != '\0') {
        return std::filesystem::path(xdgConfigHome) / "umbriel/config.toml";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config/umbriel/config.toml";
      }
      return std::filesystem::path(".config/umbriel/config.toml");
    }

    bool knownKey(std::string_view key, std::initializer_list<std::string_view> known) {
      return std::ranges::find(known, key) != known.end();
    }

    void warnUnknownKeys(
        const toml::table& section, std::string_view sectionName, std::initializer_list<std::string_view> known
    ) {
      for (const auto& [key, value] : section) {
        (void)value;
        if (!knownKey(key.str(), known)) {
          kLog.warn("config: unknown key {}.{}", sectionName, key.str());
        }
      }
    }

    void readInteger(
        const toml::table& section, std::string_view name, std::string_view fullName, int minimum, int maximum,
        int& target
    ) {
      const toml::node* node = section.get(name);
      if (node == nullptr) {
        return;
      }
      const auto value = node->value<std::int64_t>();
      if (!value) {
        kLog.warn("config: ignoring {} (expected integer)", fullName);
        return;
      }
      const std::int64_t used =
          std::clamp(*value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum));
      if (used != *value) {
        kLog.warn("config: {} = {} out of range, clamped to {}", fullName, *value, used);
      }
      target = static_cast<int>(used);
    }

    void readDouble(
        const toml::table& section, std::string_view name, std::string_view fullName, double minimum, double maximum,
        double& target
    ) {
      const toml::node* node = section.get(name);
      if (node == nullptr) {
        return;
      }
      const auto value = node->value<double>();
      if (!value || std::isnan(*value)) {
        kLog.warn("config: ignoring {} (expected number)", fullName);
        return;
      }
      const double used = std::clamp(*value, minimum, maximum);
      if (used != *value) {
        kLog.warn("config: {} = {} out of range, clamped to {}", fullName, *value, used);
      }
      target = used;
    }

    int hexDigit(char character) {
      if (character >= '0' && character <= '9') {
        return character - '0';
      }
      if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
      }
      if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
      }
      return -1;
    }

    bool parseColor(std::string_view string, std::array<float, 4>& output) {
      if ((string.size() != 7 && string.size() != 9) || string.front() != '#') {
        return false;
      }
      std::array<float, 4> parsed{0.0F, 0.0F, 0.0F, 1.0F};
      for (std::size_t component = 0; component < (string.size() - 1) / 2; ++component) {
        const int high = hexDigit(string[component * 2 + 1]);
        const int low = hexDigit(string[component * 2 + 2]);
        if (high < 0 || low < 0) {
          return false;
        }
        parsed[component] = static_cast<float>(high * 16 + low) / 255.0F;
      }
      output = parsed;
      return true;
    }

    void readColor(
        const toml::table& section, std::string_view name, std::string_view fullName, std::array<float, 4>& target
    ) {
      const toml::node* node = section.get(name);
      if (node == nullptr) {
        return;
      }
      const auto value = node->value<std::string>();
      if (!value) {
        kLog.warn("config: ignoring {} (expected color string)", fullName);
        return;
      }
      std::array<float, 4> parsed;
      if (!parseColor(*value, parsed)) {
        kLog.warn("config: ignoring {} (invalid color '{}')", fullName, *value);
        return;
      }
      target = parsed;
    }

    void readWidthPresets(const toml::table& section, std::vector<double>& target) {
      const toml::node* node = section.get("width_presets");
      if (node == nullptr) {
        return;
      }
      const auto* array = node->as_array();
      if (array == nullptr || array->empty()) {
        kLog.warn("config: ignoring layout.width_presets (expected non-empty array of numbers)");
        return;
      }

      std::vector<double> parsed;
      parsed.reserve(array->size());
      for (const auto& entry : *array) {
        const auto value = entry.value<double>();
        if (!value || std::isnan(*value)) {
          kLog.warn("config: ignoring layout.width_presets (expected non-empty array of numbers)");
          return;
        }
        const double used = std::clamp(*value, 0.1, 1.0);
        if (used != *value) {
          kLog.warn("config: layout.width_presets = {} out of range, clamped to {}", *value, used);
        }
        parsed.push_back(used);
      }
      target = std::move(parsed);
    }

    void readAppearance(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("appearance");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        kLog.warn("config: ignoring appearance (expected table)");
        return;
      }
      warnUnknownKeys(
          *section, "appearance",
          {"border_width", "corner_radius", "border_focused", "border_unfocused", "animation_ms"}
      );
      readInteger(*section, "border_width", "appearance.border_width", 0, 100, loaded.appearance.borderWidth);
      readInteger(*section, "corner_radius", "appearance.corner_radius", 0, 500, loaded.appearance.cornerRadius);
      readColor(*section, "border_focused", "appearance.border_focused", loaded.appearance.borderFocused);
      readColor(*section, "border_unfocused", "appearance.border_unfocused", loaded.appearance.borderUnfocused);
      readInteger(*section, "animation_ms", "appearance.animation_ms", 1, 10000, loaded.appearance.animationMs);
    }

    void readLayout(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("layout");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        kLog.warn("config: ignoring layout (expected table)");
        return;
      }
      warnUnknownKeys(*section, "layout", {"gap", "default_width_fraction", "width_presets", "scroll_wheel_step"});
      readInteger(*section, "gap", "layout.gap", 0, 500, loaded.layout.gap);
      readDouble(
          *section, "default_width_fraction", "layout.default_width_fraction", 0.1, 1.0,
          loaded.layout.defaultWidthFraction
      );
      readWidthPresets(*section, loaded.layout.widthPresets);
      readInteger(*section, "scroll_wheel_step", "layout.scroll_wheel_step", 1, 1000, loaded.layout.scrollWheelStep);
    }

    void readGeneral(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("general");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        kLog.warn("config: ignoring general (expected table)");
        return;
      }
      warnUnknownKeys(*section, "general", {"terminal"});
      const toml::node* terminal = section->get("terminal");
      if (terminal == nullptr) {
        return;
      }
      const auto value = terminal->value<std::string>();
      if (!value) {
        kLog.warn("config: ignoring general.terminal (expected string)");
        return;
      }
      loaded.general.terminal = *value;
    }

    void warnUnknownTopLevel(const toml::table& table) {
      for (const auto& [key, value] : table) {
        (void)value;
        if (!knownKey(key.str(), {"appearance", "layout", "general"})) {
          kLog.warn("config: unknown key {}", key.str());
        }
      }
    }

  } // namespace

  const Config& config() { return g_config; }

  void loadConfig(const char* explicitPath) {
    g_config = Config{};
    const std::filesystem::path path =
        explicitPath == nullptr ? defaultConfigPath() : std::filesystem::path(explicitPath);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
      if (explicitPath != nullptr) {
        kLog.error("config file not found: {}", path.string());
      } else {
        kLog.info("no config file found: {}, using defaults", path.string());
      }
      return;
    }

    try {
      auto result = configmerge::mergeWithIncludes(path);
      Config loaded;
      warnUnknownTopLevel(result.merged);
      readAppearance(result.merged, loaded);
      readLayout(result.merged, loaded);
      readGeneral(result.merged, loaded);
      g_config = std::move(loaded);
    } catch (const std::exception& exception) {
      kLog.error("config load error: {}", exception.what());
    } catch (...) {
      kLog.error("config load error: unknown error");
    }
  }

} // namespace umbriel
