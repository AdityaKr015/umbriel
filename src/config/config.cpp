#include "config/config.h"

#include "config/config_merge.h"
#include "core/log.h"

// clang-format off
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace umbriel {

  namespace {

    constexpr Logger kLog("config");
    Config g_config;

    std::vector<Keybind> defaultKeybinds() {
      std::vector<Keybind> keybinds;
      keybinds.reserve(59);
      auto add = [&keybinds](KeybindAction action, uint32_t keysym, uint32_t modifiers = 0) {
        keybinds.push_back({
            .modifiers = modifiers,
            .useMod = true,
            .keysym = xkb_keysym_to_lower(keysym),
            .action = action,
            .spawnCommand = {},
            .workspace = 0,
        });
      };

      add(KeybindAction::SpawnTerminal, XKB_KEY_Return);
      add(KeybindAction::Quit, XKB_KEY_Escape);
      add(KeybindAction::FocusNext, XKB_KEY_F1);

      add(KeybindAction::FocusLeft, XKB_KEY_Left);
      add(KeybindAction::FocusLeft, XKB_KEY_h);
      add(KeybindAction::FocusRight, XKB_KEY_Right);
      add(KeybindAction::FocusRight, XKB_KEY_l);
      add(KeybindAction::FocusUp, XKB_KEY_Up);
      add(KeybindAction::FocusUp, XKB_KEY_k);
      add(KeybindAction::FocusDown, XKB_KEY_Down);
      add(KeybindAction::FocusDown, XKB_KEY_j);

      add(KeybindAction::MoveColumnLeft, XKB_KEY_Left, WLR_MODIFIER_SHIFT);
      add(KeybindAction::MoveColumnLeft, XKB_KEY_h, WLR_MODIFIER_SHIFT);
      add(KeybindAction::MoveColumnRight, XKB_KEY_Right, WLR_MODIFIER_SHIFT);
      add(KeybindAction::MoveColumnRight, XKB_KEY_l, WLR_MODIFIER_SHIFT);
      add(KeybindAction::MoveUp, XKB_KEY_Up, WLR_MODIFIER_SHIFT);
      add(KeybindAction::MoveUp, XKB_KEY_k, WLR_MODIFIER_SHIFT);
      add(KeybindAction::MoveDown, XKB_KEY_Down, WLR_MODIFIER_SHIFT);
      add(KeybindAction::MoveDown, XKB_KEY_j, WLR_MODIFIER_SHIFT);

      add(KeybindAction::ConsumeLeft, XKB_KEY_comma);
      add(KeybindAction::ExpelRight, XKB_KEY_period);
      add(KeybindAction::CycleWidth, XKB_KEY_r);
      add(KeybindAction::ToggleFullWidth, XKB_KEY_f);

      for (int index = 0; index < 9; ++index) {
        const uint32_t digit = XKB_KEY_1 + static_cast<uint32_t>(index);
        const uint32_t keypad = XKB_KEY_KP_1 + static_cast<uint32_t>(index);
        auto addWorkspace = [&](KeybindAction action, uint32_t keysym, uint32_t modifiers) {
          keybinds.push_back({
              .modifiers = modifiers,
              .useMod = true,
              .keysym = keysym,
              .action = action,
              .spawnCommand = {},
              .workspace = index,
          });
        };
        addWorkspace(KeybindAction::Workspace, digit, 0);
        addWorkspace(KeybindAction::Workspace, keypad, 0);
        addWorkspace(KeybindAction::MoveToWorkspace, digit, WLR_MODIFIER_SHIFT);
        addWorkspace(KeybindAction::MoveToWorkspace, keypad, WLR_MODIFIER_SHIFT);
      }

      return keybinds;
    }

    bool parseChord(std::string_view chord, Keybind& output) {
      output = Keybind{};

      std::vector<std::string_view> tokens;
      size_t start = 0;
      while (start <= chord.size()) {
        const size_t separator = chord.find('+', start);
        const size_t end = separator == std::string_view::npos ? chord.size() : separator;
        const std::string_view token = chord.substr(start, end - start);
        if (token.empty()) {
          return false;
        }
        tokens.push_back(token);
        if (separator == std::string_view::npos) {
          break;
        }
        start = separator + 1;
      }
      if (tokens.empty()) {
        return false;
      }

      const std::string keyName(tokens.back());
      const xkb_keysym_t keysym = xkb_keysym_from_name(keyName.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
      if (keysym == XKB_KEY_NoSymbol) {
        return false;
      }

      for (size_t index = 0; index + 1 < tokens.size(); ++index) {
        std::string modifier(tokens[index]);
        std::ranges::transform(modifier, modifier.begin(), [](unsigned char character) {
          return static_cast<char>(std::tolower(character));
        });
        if (modifier == "mod") {
          output.useMod = true;
        } else if (modifier == "shift") {
          output.modifiers |= WLR_MODIFIER_SHIFT;
        } else if (modifier == "ctrl" || modifier == "control") {
          output.modifiers |= WLR_MODIFIER_CTRL;
        } else if (modifier == "alt") {
          output.modifiers |= WLR_MODIFIER_ALT;
        } else if (modifier == "super" || modifier == "logo" || modifier == "win") {
          output.modifiers |= WLR_MODIFIER_LOGO;
        } else {
          return false;
        }
      }

      output.keysym = xkb_keysym_to_lower(keysym);
      return output.useMod || output.modifiers != 0;
    }

    bool parseAction(std::string_view value, Keybind& output) {
      static constexpr std::pair<std::string_view, KeybindAction> actions[] = {
          {"none", KeybindAction::None},
          {"spawn-terminal", KeybindAction::SpawnTerminal},
          {"close", KeybindAction::Close},
          {"quit", KeybindAction::Quit},
          {"focus-left", KeybindAction::FocusLeft},
          {"focus-right", KeybindAction::FocusRight},
          {"focus-up", KeybindAction::FocusUp},
          {"focus-down", KeybindAction::FocusDown},
          {"move-column-left", KeybindAction::MoveColumnLeft},
          {"move-column-right", KeybindAction::MoveColumnRight},
          {"move-up", KeybindAction::MoveUp},
          {"move-down", KeybindAction::MoveDown},
          {"consume-left", KeybindAction::ConsumeLeft},
          {"expel-right", KeybindAction::ExpelRight},
          {"cycle-width", KeybindAction::CycleWidth},
          {"toggle-full-width", KeybindAction::ToggleFullWidth},
          {"focus-next", KeybindAction::FocusNext},
      };
      for (const auto& [name, action] : actions) {
        if (value == name) {
          output.action = action;
          return true;
        }
      }

      constexpr std::string_view spawnPrefix = "spawn:";
      if (value.starts_with(spawnPrefix)) {
        output.action = KeybindAction::Spawn;
        output.spawnCommand = value.substr(spawnPrefix.size());
        return true;
      }

      auto parseWorkspace = [&](std::string_view prefix, KeybindAction action) {
        if (!value.starts_with(prefix)) {
          return false;
        }
        const std::string_view number = value.substr(prefix.size());
        if (number.size() != 1 || number.front() < '1' || number.front() > '9') {
          return false;
        }
        output.action = action;
        output.workspace = number.front() - '1';
        return true;
      };
      return parseWorkspace("workspace:", KeybindAction::Workspace)
          || parseWorkspace("move-to-workspace:", KeybindAction::MoveToWorkspace);
    }

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

    void readKeybinds(const toml::table& table, Config& loaded) {
      loaded.keybinds = defaultKeybinds();
      const toml::node* node = table.get("keybinds");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        kLog.warn("config: ignoring keybinds (expected table)");
        return;
      }

      std::vector<Keybind> configured;
      auto sameChord = [](const Keybind& left, const Keybind& right) {
        return left.modifiers == right.modifiers && left.useMod == right.useMod && left.keysym == right.keysym;
      };
      for (const auto& [key, entry] : *section) {
        const std::string chord(key.str());
        const auto value = entry.value<std::string>();
        if (!value) {
          kLog.warn("config: ignoring keybind '{}' (expected string)", chord);
          continue;
        }

        Keybind binding;
        if (!parseChord(chord, binding)) {
          if (binding.keysym != XKB_KEY_NoSymbol && binding.modifiers == 0 && !binding.useMod) {
            kLog.warn("config: ignoring keybind '{}' (needs at least one modifier)", chord);
          } else {
            kLog.warn("config: ignoring keybind '{}' (bad chord)", chord);
          }
          continue;
        }
        if (!parseAction(*value, binding)) {
          kLog.warn("config: ignoring keybind '{}' (unknown action '{}')", chord, *value);
          continue;
        }

        if (std::ranges::any_of(configured, [&](const Keybind& existing) { return sameChord(existing, binding); })) {
          kLog.warn("config: duplicate keybind {}", chord);
        }
        std::erase_if(configured, [&](const Keybind& existing) { return sameChord(existing, binding); });
        configured.push_back(binding);
        std::erase_if(loaded.keybinds, [&](const Keybind& existing) { return sameChord(existing, binding); });
        if (binding.action != KeybindAction::None) {
          loaded.keybinds.push_back(std::move(binding));
        }
      }
    }

    void warnUnknownTopLevel(const toml::table& table) {
      for (const auto& [key, value] : table) {
        (void)value;
        if (!knownKey(key.str(), {"appearance", "layout", "general", "keybinds"})) {
          kLog.warn("config: unknown key {}", key.str());
        }
      }
    }

  } // namespace

  const Config& config() { return g_config; }

  void loadConfig(const char* explicitPath) {
    g_config = Config{};
    g_config.keybinds = defaultKeybinds();
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
      readKeybinds(result.merged, loaded);
      g_config = std::move(loaded);
    } catch (const std::exception& exception) {
      kLog.error("config load error: {}", exception.what());
    } catch (...) {
      kLog.error("config load error: unknown error");
    }
  }

} // namespace umbriel
