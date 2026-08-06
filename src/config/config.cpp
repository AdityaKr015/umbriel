#include "config/config.h"

#include "config/config_diag.h"
#include "config/config_merge.h"
#include "core/log.h"

// clang-format off
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <utility>

namespace umbriel {

  namespace {

    constexpr Logger kLog("config");
    Config g_config;
    std::filesystem::path g_rootPath;
    bool g_explicitPath = false;
    std::vector<std::filesystem::path> g_watchPaths;
    std::vector<ConfigDiagnostic> g_diagnostics;

    void emitDiag(ConfigDiagnostic::Severity severity, const toml::source_region* src, std::string msg) {
      ConfigDiagnostic diag;
      diag.severity = severity;
      diag.message = msg;
      if (src != nullptr) {
        diag.line = src->begin.line;
        diag.column = src->begin.column;
        if (src->path != nullptr) {
          diag.file = *src->path;
        }
      }
      const std::string loc = diag.location();
      if (severity == ConfigDiagnostic::Severity::Error) {
        kLog.error("{}{}", loc.empty() ? "" : loc + ": ", msg);
      } else {
        kLog.warn("{}{}", loc.empty() ? "" : loc + ": ", msg);
      }
      g_diagnostics.push_back(std::move(diag));
    }

    template <typename... A> void warnAt(const toml::source_region& src, std::format_string<A...> fmt, A&&... args) {
      emitDiag(ConfigDiagnostic::Severity::Warning, &src, std::format(fmt, std::forward<A>(args)...));
    }

    template <typename... A> void warnNoSrc(std::format_string<A...> fmt, A&&... args) {
      emitDiag(ConfigDiagnostic::Severity::Warning, nullptr, std::format(fmt, std::forward<A>(args)...));
    }

    template <typename... A> void errorAt(const toml::source_region& src, std::format_string<A...> fmt, A&&... args) {
      emitDiag(ConfigDiagnostic::Severity::Error, &src, std::format(fmt, std::forward<A>(args)...));
    }

    std::vector<Keybind> defaultKeybinds() {
      std::vector<Keybind> keybinds;
      keybinds.reserve(60);
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

      add(KeybindAction::SessionQuit, XKB_KEY_Escape);
      add(KeybindAction::WindowFocusNext, XKB_KEY_F1);

      add(KeybindAction::WindowFocusLeft, XKB_KEY_Left);
      add(KeybindAction::WindowFocusLeft, XKB_KEY_h);
      add(KeybindAction::WindowFocusRight, XKB_KEY_Right);
      add(KeybindAction::WindowFocusRight, XKB_KEY_l);
      add(KeybindAction::WindowFocusUp, XKB_KEY_Up);
      add(KeybindAction::WindowFocusUp, XKB_KEY_k);
      add(KeybindAction::WindowFocusDown, XKB_KEY_Down);
      add(KeybindAction::WindowFocusDown, XKB_KEY_j);

      add(KeybindAction::ColumnMoveLeft, XKB_KEY_Left, WLR_MODIFIER_SHIFT);
      add(KeybindAction::ColumnMoveLeft, XKB_KEY_h, WLR_MODIFIER_SHIFT);
      add(KeybindAction::ColumnMoveRight, XKB_KEY_Right, WLR_MODIFIER_SHIFT);
      add(KeybindAction::ColumnMoveRight, XKB_KEY_l, WLR_MODIFIER_SHIFT);
      add(KeybindAction::WindowMoveUp, XKB_KEY_Up, WLR_MODIFIER_SHIFT);
      add(KeybindAction::WindowMoveUp, XKB_KEY_k, WLR_MODIFIER_SHIFT);
      add(KeybindAction::WindowMoveDown, XKB_KEY_Down, WLR_MODIFIER_SHIFT);
      add(KeybindAction::WindowMoveDown, XKB_KEY_j, WLR_MODIFIER_SHIFT);

      add(KeybindAction::WindowConsumeLeft, XKB_KEY_comma);
      add(KeybindAction::WindowExpelRight, XKB_KEY_period);
      add(KeybindAction::WindowCycleWidth, XKB_KEY_r);
      add(KeybindAction::ToggleFullscreen, XKB_KEY_f);
      add(KeybindAction::ToggleMaximize, XKB_KEY_f, WLR_MODIFIER_CTRL);
      add(KeybindAction::ToggleFloating, XKB_KEY_t);

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
        addWorkspace(KeybindAction::WorkspaceSwitch, digit, 0);
        addWorkspace(KeybindAction::WorkspaceSwitch, keypad, 0);
        addWorkspace(KeybindAction::WindowMoveToWorkspace, digit, WLR_MODIFIER_SHIFT);
        addWorkspace(KeybindAction::WindowMoveToWorkspace, keypad, WLR_MODIFIER_SHIFT);
      }

      // Default wheel binds: Mod+WheelUp = window-focus-left, Mod+WheelDown = window-focus-right.
      keybinds.push_back(
          {.modifiers = 0,
           .useMod = true,
           .keysym = 0,
           .wheel = WheelDirection::Up,
           .action = KeybindAction::WindowFocusLeft,
           .spawnCommand = {},
           .workspace = 0}
      );
      keybinds.push_back(
          {.modifiers = 0,
           .useMod = true,
           .keysym = 0,
           .wheel = WheelDirection::Down,
           .action = KeybindAction::WindowFocusRight,
           .spawnCommand = {},
           .workspace = 0}
      );

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

      // Check if the last token is a wheel direction.
      std::string lastLower(tokens.back());
      std::ranges::transform(lastLower, lastLower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      WheelDirection wheelDir = WheelDirection::None;
      if (lastLower == "wheelup") {
        wheelDir = WheelDirection::Up;
      } else if (lastLower == "wheeldown") {
        wheelDir = WheelDirection::Down;
      } else if (lastLower == "wheelleft") {
        wheelDir = WheelDirection::Left;
      } else if (lastLower == "wheelright") {
        wheelDir = WheelDirection::Right;
      }

      if (wheelDir != WheelDirection::None) {
        // A bare wheel bind (no modifier) would hijack all client scrolling.
        if (tokens.size() < 2) {
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
        output.wheel = wheelDir;
        return true;
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
      return true;
    }

  } // namespace

  static constexpr ActionSpec kActionSpecs[] = {
      {"column-move-left", "", KeybindAction::ColumnMoveLeft},
      {"column-move-right", "", KeybindAction::ColumnMoveRight},
      {"config-reload", "", KeybindAction::ConfigReload},
      {"layout-scroll-left", "", KeybindAction::LayoutScrollLeft},
      {"layout-scroll-right", "", KeybindAction::LayoutScrollRight},
      {"none", "", KeybindAction::None},
      {"session-quit", "", KeybindAction::SessionQuit},
      {"spawn", "<cmd>", KeybindAction::Spawn, ActionArgKind::Command},
      {"window-close", "", KeybindAction::WindowClose},
      {"window-consume-left", "", KeybindAction::WindowConsumeLeft},
      {"window-cycle-width", "", KeybindAction::WindowCycleWidth},
      {"window-expel-right", "", KeybindAction::WindowExpelRight},
      {"window-focus-down", "", KeybindAction::WindowFocusDown},
      {"window-focus-left", "", KeybindAction::WindowFocusLeft},
      {"window-focus-next", "", KeybindAction::WindowFocusNext},
      {"window-focus-right", "", KeybindAction::WindowFocusRight},
      {"window-focus-up", "", KeybindAction::WindowFocusUp},
      {"window-move-down", "", KeybindAction::WindowMoveDown},
      {"window-move-to-workspace", "<1-9>", KeybindAction::WindowMoveToWorkspace, ActionArgKind::Workspace},
      {"window-move-up", "", KeybindAction::WindowMoveUp},
      {"window-toggle-floating", "", KeybindAction::ToggleFloating},
      {"window-toggle-fullscreen", "", KeybindAction::ToggleFullscreen},
      {"window-toggle-maximize", "", KeybindAction::ToggleMaximize},
      {"workspace-switch", "<1-9>", KeybindAction::WorkspaceSwitch, ActionArgKind::Workspace},
  };

  std::span<const ActionSpec> actionSpecs() { return kActionSpecs; }

  bool parseAction(std::string_view value, Keybind& output) {
    for (const auto& spec : kActionSpecs) {
      switch (spec.argKind) {
      case ActionArgKind::None:
        if (value == spec.name) {
          output.action = spec.action;
          return true;
        }
        break;
      case ActionArgKind::Command: {
        // Match "name:arg"
        if (value.size() > spec.name.size() + 1
            && value[spec.name.size()] == ':'
            && value.substr(0, spec.name.size()) == spec.name) {
          output.action = spec.action;
          output.spawnCommand = value.substr(spec.name.size() + 1);
          return true;
        }
        break;
      }
      case ActionArgKind::Workspace: {
        // Match "name:N" where N is 1-9
        if (value.size() == spec.name.size() + 2
            && value[spec.name.size()] == ':'
            && value.substr(0, spec.name.size()) == spec.name) {
          char ch = value.back();
          if (ch >= '1' && ch <= '9') {
            output.action = spec.action;
            output.workspace = ch - '1';
            return true;
          }
        }
        break;
      }
      }
    }
    return false;
  }

  namespace {

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
    bool parseOutputMode(std::string_view value, OutputMode& output) {
      const size_t widthEnd = value.find('x');
      if (widthEnd == std::string_view::npos) {
        return false;
      }
      const size_t heightEnd = value.find('@', widthEnd + 1);
      const std::string_view widthText = value.substr(0, widthEnd);
      const std::string_view heightText =
          value.substr(widthEnd + 1, heightEnd == std::string_view::npos ? value.size() : heightEnd - widthEnd - 1);
      if (widthText.empty() || heightText.empty()) {
        return false;
      }

      int width = 0;
      int height = 0;
      const auto [widthPtr, widthError] = std::from_chars(widthText.data(), widthText.data() + widthText.size(), width);
      const auto [heightPtr, heightError] =
          std::from_chars(heightText.data(), heightText.data() + heightText.size(), height);
      if (widthError != std::errc{}
          || widthPtr != widthText.data() + widthText.size()
          || heightError != std::errc{}
          || heightPtr != heightText.data() + heightText.size()) {
        return false;
      }

      double refreshHz = 0.0;
      if (heightEnd != std::string_view::npos) {
        const std::string_view refreshText = value.substr(heightEnd + 1);
        if (refreshText.empty()) {
          return false;
        }
        const auto [refreshPtr, refreshError] =
            std::from_chars(refreshText.data(), refreshText.data() + refreshText.size(), refreshHz);
        if (refreshError != std::errc{}
            || refreshPtr != refreshText.data() + refreshText.size()
            || !std::isfinite(refreshHz)) {
          return false;
        }
      }

      output.width = std::clamp(width, 1, 16384);
      output.height = std::clamp(height, 1, 16384);
      output.refreshMHz = static_cast<int>(std::lround(std::clamp(refreshHz, 0.0, 1000.0) * 1000.0));
      return true;
    }

    void warnUnknownKeys(
        const toml::table& section, std::string_view sectionName, std::initializer_list<std::string_view> known
    ) {
      for (const auto& [key, value] : section) {
        (void)value;
        if (!knownKey(key.str(), known)) {
          warnAt(key.source(), "unknown key {}.{}", sectionName, key.str());
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
        warnAt(node->source(), "ignoring {} (expected integer)", fullName);
        return;
      }
      const std::int64_t used =
          std::clamp(*value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum));
      if (used != *value) {
        warnAt(node->source(), "{} = {} out of range, clamped to {}", fullName, *value, used);
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
        warnAt(node->source(), "ignoring {} (expected number)", fullName);
        return;
      }
      const double used = std::clamp(*value, minimum, maximum);
      if (used != *value) {
        warnAt(node->source(), "{} = {} out of range, clamped to {}", fullName, *value, used);
      }
      target = used;
    }

    void readString(const toml::table& section, std::string_view name, std::string_view fullName, std::string& target) {
      const toml::node* node = section.get(name);
      if (node == nullptr) {
        return;
      }
      const auto value = node->value<std::string>();
      if (!value) {
        warnAt(node->source(), "ignoring {} (expected string)", fullName);
        return;
      }
      target = *value;
    }

    void readBoolean(
        const toml::table& section, std::string_view name, std::string_view fullName, std::optional<bool>& target
    ) {
      const toml::node* node = section.get(name);
      if (node == nullptr) {
        return;
      }
      if (!node->is_boolean()) {
        warnAt(node->source(), "ignoring {} (expected boolean)", fullName);
        return;
      }
      target = node->value<bool>();
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
        warnAt(node->source(), "ignoring {} (expected color string)", fullName);
        return;
      }
      std::array<float, 4> parsed;
      if (!parseColor(*value, parsed)) {
        warnAt(node->source(), "ignoring {} (invalid color '{}')", fullName, *value);
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
        warnAt(node->source(), "ignoring layout.width_presets (expected non-empty array of numbers)");
        return;
      }

      std::vector<double> parsed;
      parsed.reserve(array->size());
      for (const auto& entry : *array) {
        const auto value = entry.value<double>();
        if (!value || std::isnan(*value)) {
          warnAt(node->source(), "ignoring layout.width_presets (expected non-empty array of numbers)");
          return;
        }
        const double used = std::clamp(*value, 0.1, 1.0);
        if (used != *value) {
          warnAt(entry.source(), "layout.width_presets = {} out of range, clamped to {}", *value, used);
        }
        parsed.push_back(used);
      }
      target = std::move(parsed);
    }

    void readWidthPresetsInto(
        const toml::table& section, std::string_view context, std::optional<std::vector<double>>& target
    ) {
      const toml::node* node = section.get("width_presets");
      if (node == nullptr) {
        return;
      }
      const auto* array = node->as_array();
      if (array == nullptr || array->empty()) {
        warnAt(node->source(), "ignoring {}.width_presets (expected non-empty array of numbers)", context);
        return;
      }
      std::vector<double> parsed;
      parsed.reserve(array->size());
      for (const auto& entry : *array) {
        const auto value = entry.value<double>();
        if (!value || std::isnan(*value)) {
          warnAt(node->source(), "ignoring {}.width_presets (expected non-empty array of numbers)", context);
          return;
        }
        const double used = std::clamp(*value, 0.1, 1.0);
        if (used != *value) {
          warnAt(entry.source(), "{}.width_presets = {} out of range, clamped to {}", context, *value, used);
        }
        parsed.push_back(used);
      }
      target = std::move(parsed);
    }

    void readOptionalInt(
        const toml::table& section, std::string_view name, std::string_view fullName, int minimum, int maximum,
        std::optional<int>& target
    ) {
      const toml::node* node = section.get(name);
      if (node == nullptr) {
        return;
      }
      const auto value = node->value<std::int64_t>();
      if (!value) {
        warnAt(node->source(), "ignoring {} (expected integer)", fullName);
        return;
      }
      const std::int64_t used =
          std::clamp(*value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum));
      if (used != *value) {
        warnAt(node->source(), "{} = {} out of range, clamped to {}", fullName, *value, used);
      }
      target = static_cast<int>(used);
    }

    void readOptionalDouble(
        const toml::table& section, std::string_view name, std::string_view fullName, double minimum, double maximum,
        std::optional<double>& target
    ) {
      const toml::node* node = section.get(name);
      if (node == nullptr) {
        return;
      }
      const auto value = node->value<double>();
      if (!value || std::isnan(*value)) {
        warnAt(node->source(), "ignoring {} (expected number)", fullName);
        return;
      }
      const double used = std::clamp(*value, minimum, maximum);
      if (used != *value) {
        warnAt(node->source(), "{} = {} out of range, clamped to {}", fullName, *value, used);
      }
      target = used;
    }

    void readWorkspaceLayoutOverrides(
        const toml::table& section, std::string_view context, WorkspaceLayoutOverrides& overrides
    ) {
      const toml::node* layoutNode = section.get("layout");
      if (layoutNode == nullptr) {
        return;
      }
      const auto* layout = layoutNode->as_table();
      if (layout == nullptr) {
        warnAt(layoutNode->source(), "ignoring {}.layout (expected table)", context);
        return;
      }
      warnUnknownKeys(
          *layout, std::string(context) + ".layout", {"mode", "gap", "default_width_fraction", "width_presets"}
      );
      if (const toml::node* modeNode = layout->get("mode")) {
        if (const auto* modeStr = modeNode->as_string()) {
          const std::string_view sv = modeStr->get();
          if (sv == "dwindle") {
            overrides.mode = LayoutMode::Dwindle;
          } else if (sv == "scrolling") {
            overrides.mode = LayoutMode::Scrolling;
          } else {
            warnAt(
                modeNode->source(), R"(unknown {}.layout.mode "{}" (expected "scrolling" or "dwindle"))", context, sv
            );
          }
        } else {
          warnAt(modeNode->source(), R"({}.layout.mode must be a string ("scrolling" or "dwindle"))", context);
        }
      }
      const std::string ctx(context);
      readOptionalInt(*layout, "gap", ctx + ".layout.gap", 0, 500, overrides.gap);
      readOptionalDouble(
          *layout, "default_width_fraction", ctx + ".layout.default_width_fraction", 0.1, 1.0,
          overrides.defaultWidthFraction
      );
      readWidthPresetsInto(*layout, ctx + ".layout", overrides.widthPresets);
    }

    WorkspaceConfig parseWorkspaceEntry(const toml::table& section, std::string_view context) {
      WorkspaceConfig ws;
      warnUnknownKeys(section, context, {"name", "output", "index", "append", "layout"});

      if (const toml::node* nameNode = section.get("name")) {
        if (const auto value = nameNode->value<std::string>()) {
          ws.name = *value;
        } else {
          warnAt(nameNode->source(), "ignoring {}.name (expected string)", context);
        }
      }
      if (const toml::node* outputNode = section.get("output")) {
        if (const auto value = outputNode->value<std::string>()) {
          if (value->empty()) {
            errorAt(outputNode->source(), "{}.output must not be empty", context);
          } else {
            ws.output = *value;
          }
        } else {
          warnAt(outputNode->source(), "ignoring {}.output (expected string)", context);
        }
      }
      if (const toml::node* indexNode = section.get("index")) {
        const auto value = indexNode->value<std::int64_t>();
        if (!value || *value < 1) {
          warnAt(indexNode->source(), "ignoring {}.index (expected positive integer)", context);
        } else {
          ws.index = static_cast<int>(*value);
        }
      }
      if (const toml::node* appendNode = section.get("append")) {
        if (const auto value = appendNode->value<bool>()) {
          ws.append = *value;
        } else {
          warnAt(appendNode->source(), "ignoring {}.append (expected boolean)", context);
        }
      }

      readWorkspaceLayoutOverrides(section, context, ws.layout);
      return ws;
    }

    void readWorkspaces(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("workspace");
      if (node == nullptr) {
        return;
      }
      const auto* workspaces = node->as_array();
      if (workspaces == nullptr) {
        warnAt(node->source(), "ignoring workspace (expected [[workspace]] array of tables)");
        return;
      }

      // Pass 1: parse all entries and partition into base / output-qualified.
      struct ParsedEntry {
        WorkspaceConfig ws;
        toml::source_region source;
        int arrayIndex;
      };
      std::vector<ParsedEntry> baseEntries;
      std::vector<ParsedEntry> outputEntries;

      int entryIndex = 0;
      for (const auto& entry : *workspaces) {
        const auto* section = entry.as_table();
        if (section == nullptr) {
          warnAt(entry.source(), "ignoring workspace entry (expected table)");
          ++entryIndex;
          continue;
        }
        const std::string context = std::format("workspace[{}]", entryIndex);
        WorkspaceConfig ws = parseWorkspaceEntry(*section, context);

        if (ws.output.empty()) {
          // Base entry: reject base-invalid fields.
          if (ws.index.has_value()) {
            errorAt(entry.source(), "{}: index is only valid on output-qualified entries", context);
            ++entryIndex;
            continue;
          }
          if (ws.append) {
            errorAt(entry.source(), "{}: append is only valid on output-qualified entries", context);
            ++entryIndex;
            continue;
          }
          baseEntries.push_back({std::move(ws), entry.source(), entryIndex});
        } else {
          // Output-qualified: reject append+index combo early.
          if (ws.append && ws.index.has_value()) {
            errorAt(entry.source(), "{}: append and index are mutually exclusive", context);
            ++entryIndex;
            continue;
          }
          if (!ws.append && !ws.index.has_value() && ws.name.empty()) {
            errorAt(entry.source(), "{}: output entry needs name, index, or append = true", context);
            ++entryIndex;
            continue;
          }
          outputEntries.push_back({std::move(ws), entry.source(), entryIndex});
        }
        ++entryIndex;
      }

      // Pass 2: validate base entries (duplicate names).
      for (size_t i = 0; i < baseEntries.size(); ++i) {
        if (baseEntries[i].ws.name.empty()) {
          continue;
        }
        for (size_t j = 0; j < i; ++j) {
          if (baseEntries[j].ws.name == baseEntries[i].ws.name) {
            errorAt(baseEntries[i].source, "duplicate base workspace name '{}'", baseEntries[i].ws.name);
            break;
          }
        }
      }

      // Pass 3: validate output-qualified entries against the complete base set.
      const int baseCount = static_cast<int>(baseEntries.size());
      for (size_t i = 0; i < outputEntries.size(); ++i) {
        auto& oe = outputEntries[i];
        const std::string context = std::format("workspace[{}]", oe.arrayIndex);

        if (!oe.ws.append && oe.ws.index.has_value()) {
          if (*oe.ws.index < 1 || *oe.ws.index > baseCount) {
            errorAt(
                oe.source, "{}: index {} does not reference a base workspace (have {})", context, *oe.ws.index,
                baseCount
            );
            continue;
          }
          // Check duplicate (output, index).
          for (size_t j = 0; j < i; ++j) {
            if (outputEntries[j].ws.output == oe.ws.output && outputEntries[j].ws.index == oe.ws.index) {
              errorAt(oe.source, "duplicate override for output '{}' index {}", oe.ws.output, *oe.ws.index);
              break;
            }
          }
        } else if (!oe.ws.append && !oe.ws.name.empty()) {
          // Name selector: must match a base entry name.
          bool found = false;
          for (const auto& base : baseEntries) {
            if (base.ws.name == oe.ws.name) {
              found = true;
              break;
            }
          }
          if (!found) {
            errorAt(
                oe.source, "{}: no base workspace named '{}' (use append = true to add a new workspace)", context,
                oe.ws.name
            );
            continue;
          }
          // Check duplicate (output, name) overlay.
          for (size_t j = 0; j < i; ++j) {
            if (outputEntries[j].ws.output == oe.ws.output
                && !outputEntries[j].ws.append
                && outputEntries[j].ws.name == oe.ws.name
                && !outputEntries[j].ws.index.has_value()) {
              errorAt(oe.source, "duplicate override for output '{}' workspace '{}'", oe.ws.output, oe.ws.name);
              break;
            }
          }
        }
        // append entries need no selector validation
      }

      // Pass 4: simulate final workspace names per output, check uniqueness.
      // Collect distinct output names from output-qualified entries.
      std::vector<std::string> outputNames;
      for (const auto& oe : outputEntries) {
        if (std::ranges::find(outputNames, oe.ws.output) == outputNames.end()) {
          outputNames.push_back(oe.ws.output);
        }
      }
      // Also check base-only (outputs with no overrides).
      outputNames.push_back(std::string{}); // sentinel for "any unqualified output"

      for (const auto& outName : outputNames) {
        // Build final name list: start with base names.
        std::vector<std::string> finalNames;
        finalNames.reserve(baseEntries.size());
        for (size_t i = 0; i < baseEntries.size(); ++i) {
          finalNames.push_back(baseEntries[i].ws.name.empty() ? std::to_string(i + 1) : baseEntries[i].ws.name);
        }
        // Apply output-qualified entries for this output.
        if (!outName.empty()) {
          for (const auto& oe : outputEntries) {
            if (oe.ws.output != outName) {
              continue;
            }
            if (!oe.ws.append && oe.ws.index.has_value()) {
              const int idx = *oe.ws.index - 1;
              if (idx >= 0 && idx < static_cast<int>(finalNames.size()) && !oe.ws.name.empty()) {
                finalNames[static_cast<size_t>(idx)] = oe.ws.name;
              }
            } else if (oe.ws.append) {
              finalNames.push_back(oe.ws.name.empty() ? std::to_string(finalNames.size() + 1) : oe.ws.name);
            }
            // name overlays don't change the display name
          }
        }
        // Check for duplicates.
        for (size_t i = 0; i < finalNames.size(); ++i) {
          for (size_t j = i + 1; j < finalNames.size(); ++j) {
            if (finalNames[i] == finalNames[j]) {
              const std::string target =
                  outName.empty() ? "base workspace set" : std::string("output '") + outName + "'";
              errorAt(
                  node->source(), "{}: duplicate resolved workspace name '{}' at positions {} and {}", target,
                  finalNames[i], i + 1, j + 1
              );
              // Only report each name once per output.
              goto nextOutput;
            }
          }
        }
      nextOutput:;
      }

      // Commit all parsed entries (errors prevent config installation via parseInto).
      for (auto& e : baseEntries) {
        loaded.workspaces.push_back(std::move(e.ws));
      }
      for (auto& e : outputEntries) {
        loaded.workspaces.push_back(std::move(e.ws));
      }
    }

    void readAppearance(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("appearance");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        warnAt(node->source(), "ignoring appearance (expected table)");
        return;
      }
      warnUnknownKeys(
          *section, "appearance",
          {"border_width", "outer_border_width", "corner_radius", "border_focused", "border_unfocused",
           "outer_border_color", "insert_hint_color", "animation_ms", "blur", "shadow", "prefer_no_csd"}
      );
      readInteger(*section, "border_width", "appearance.border_width", 0, 100, loaded.appearance.borderWidth);
      readInteger(
          *section, "outer_border_width", "appearance.outer_border_width", 0, 100, loaded.appearance.outerBorderWidth
      );
      readInteger(*section, "corner_radius", "appearance.corner_radius", 0, 500, loaded.appearance.cornerRadius);
      readColor(*section, "border_focused", "appearance.border_focused", loaded.appearance.borderFocused);
      readColor(*section, "border_unfocused", "appearance.border_unfocused", loaded.appearance.borderUnfocused);
      readColor(*section, "outer_border_color", "appearance.outer_border_color", loaded.appearance.outerBorderColor);
      readColor(*section, "insert_hint_color", "appearance.insert_hint_color", loaded.appearance.insertHintColor);
      readInteger(*section, "animation_ms", "appearance.animation_ms", 1, 10000, loaded.appearance.animationMs);
      if (const toml::node* preferNoCsd = section->get("prefer_no_csd")) {
        if (const auto value = preferNoCsd->value<bool>()) {
          loaded.appearance.preferNoCsd = *value;
        } else {
          warnAt(preferNoCsd->source(), "ignoring appearance.prefer_no_csd (expected boolean)");
        }
      }

      if (const toml::node* blurNode = section->get("blur")) {
        if (const auto* blur = blurNode->as_table()) {
          warnUnknownKeys(
              *blur, "appearance.blur",
              {"enabled", "passes", "radius", "noise", "brightness", "contrast", "saturation", "ignore_alpha"}
          );
          if (const toml::node* enabledNode = blur->get("enabled")) {
            if (enabledNode->is_boolean()) {
              loaded.appearance.blur.enabled = enabledNode->value<bool>().value();
            } else {
              warnAt(enabledNode->source(), "ignoring appearance.blur.enabled (expected boolean)");
            }
          }
          readInteger(*blur, "passes", "appearance.blur.passes", 0, 8, loaded.appearance.blur.passes);
          readInteger(*blur, "radius", "appearance.blur.radius", 0, 100, loaded.appearance.blur.radius);
          readDouble(*blur, "noise", "appearance.blur.noise", 0.0, 1.0, loaded.appearance.blur.noise);
          readDouble(*blur, "brightness", "appearance.blur.brightness", 0.0, 2.0, loaded.appearance.blur.brightness);
          readDouble(*blur, "contrast", "appearance.blur.contrast", 0.0, 2.0, loaded.appearance.blur.contrast);
          readDouble(*blur, "saturation", "appearance.blur.saturation", 0.0, 2.0, loaded.appearance.blur.saturation);
          readDouble(
              *blur, "ignore_alpha", "appearance.blur.ignore_alpha", 0.0, 1.0, loaded.appearance.blur.ignoreAlpha
          );
        } else {
          warnAt(blurNode->source(), "ignoring appearance.blur (expected table)");
        }
      }

      if (const toml::node* shadowNode = section->get("shadow")) {
        if (const auto* shadow = shadowNode->as_table()) {
          warnUnknownKeys(*shadow, "appearance.shadow", {"enabled", "softness", "offset_x", "offset_y", "color"});
          if (const toml::node* enabledNode = shadow->get("enabled")) {
            if (enabledNode->is_boolean()) {
              loaded.appearance.shadow.enabled = enabledNode->value<bool>().value();
            } else {
              warnAt(enabledNode->source(), "ignoring appearance.shadow.enabled (expected boolean)");
            }
          }
          readInteger(*shadow, "softness", "appearance.shadow.softness", 0, 200, loaded.appearance.shadow.softness);
          readInteger(*shadow, "offset_x", "appearance.shadow.offset_x", -200, 200, loaded.appearance.shadow.offsetX);
          readInteger(*shadow, "offset_y", "appearance.shadow.offset_y", -200, 200, loaded.appearance.shadow.offsetY);
          readColor(*shadow, "color", "appearance.shadow.color", loaded.appearance.shadow.color);
        } else {
          warnAt(shadowNode->source(), "ignoring appearance.shadow (expected table)");
        }
      }
    }

    void readLayout(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("layout");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        warnAt(node->source(), "ignoring layout (expected table)");
        return;
      }
      warnUnknownKeys(*section, "layout", {"mode", "gap", "default_width_fraction", "width_presets"});
      if (const toml::node* modeNode = section->get("mode")) {
        if (const auto* modeStr = modeNode->as_string()) {
          const std::string_view sv = modeStr->get();
          if (sv == "dwindle") {
            loaded.layout.mode = LayoutMode::Dwindle;
          } else if (sv == "scrolling") {
            loaded.layout.mode = LayoutMode::Scrolling;
          } else {
            warnAt(modeNode->source(), R"(unknown layout.mode "{}" (expected "scrolling" or "dwindle"))", sv);
          }
        } else {
          warnAt(modeNode->source(), R"(layout.mode must be a string ("scrolling" or "dwindle"))");
        }
      }
      readInteger(*section, "gap", "layout.gap", 0, 500, loaded.layout.gap);
      readDouble(
          *section, "default_width_fraction", "layout.default_width_fraction", 0.1, 1.0,
          loaded.layout.defaultWidthFraction
      );
      readWidthPresets(*section, loaded.layout.widthPresets);
    }

    void readGeneral(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("general");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        warnAt(node->source(), "ignoring general (expected table)");
        return;
      }
      warnUnknownKeys(*section, "general", {"autostart", "prefer_no_csd", "workspace_back_and_forth", "xwayland"});
      if (const toml::node* preferNoCsd = section->get("prefer_no_csd")) {
        if (const auto value = preferNoCsd->value<bool>()) {
          loaded.appearance.preferNoCsd = *value;
          warnAt(preferNoCsd->source(), "general.prefer_no_csd is deprecated; use appearance.prefer_no_csd");
        } else {
          warnAt(preferNoCsd->source(), "ignoring general.prefer_no_csd (expected boolean)");
        }
      }
      if (const toml::node* backAndForth = section->get("workspace_back_and_forth")) {
        if (const auto value = backAndForth->value<bool>()) {
          loaded.general.workspaceBackAndForth = *value;
        } else {
          warnAt(backAndForth->source(), "ignoring general.workspace_back_and_forth (expected boolean)");
        }
      }
      if (const toml::node* xwayland = section->get("xwayland")) {
        if (const auto value = xwayland->value<bool>()) {
          loaded.general.xwayland = *value;
        } else {
          warnAt(xwayland->source(), "ignoring general.xwayland (expected boolean)");
        }
      }

      const toml::node* autostart = section->get("autostart");
      if (autostart == nullptr) {
        return;
      }
      const auto* array = autostart->as_array();
      if (array == nullptr) {
        warnAt(autostart->source(), "ignoring general.autostart (expected array of strings)");
        return;
      }
      std::vector<std::string> parsed;
      parsed.reserve(array->size());
      for (const auto& entry : *array) {
        const auto value = entry.value<std::string>();
        if (!value) {
          warnAt(entry.source(), "ignoring general.autostart (expected array of strings)");
          return;
        }
        if (value->empty()) {
          warnAt(entry.source(), "ignoring empty general.autostart entry");
          continue;
        }
        parsed.push_back(*value);
      }
      loaded.general.autostart = std::move(parsed);
    }

    void validateKeyboardInput(Config::Input::Keyboard& keyboard, const toml::source_region& source) {
      if (keyboard.layout.empty() && keyboard.variant.empty()) {
        return;
      }
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (context == nullptr) {
        warnAt(source, "unable to validate input.keyboard XKB configuration");
        keyboard.layout.clear();
        keyboard.variant.clear();
        return;
      }
      const xkb_rule_names names{
          .rules = nullptr,
          .model = nullptr,
          .layout = keyboard.layout.empty() ? nullptr : keyboard.layout.c_str(),
          .variant = keyboard.variant.empty() ? nullptr : keyboard.variant.c_str(),
          .options = nullptr,
      };
      xkb_keymap* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (keymap == nullptr) {
        warnAt(
            source, "ignoring input.keyboard layout='{}' variant='{}' (invalid XKB configuration)", keyboard.layout,
            keyboard.variant
        );
        keyboard.layout.clear();
        keyboard.variant.clear();
      } else {
        xkb_keymap_unref(keymap);
      }
      xkb_context_unref(context);
    }

    void readInput(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("input");
      if (node == nullptr) {
        return;
      }
      const auto* input = node->as_table();
      if (input == nullptr) {
        warnAt(node->source(), "ignoring input (expected table)");
        return;
      }
      warnUnknownKeys(*input, "input", {"keyboard", "touchpad", "mouse", "cursor", "focus"});

      if (const toml::node* keyboardNode = input->get("keyboard")) {
        if (const auto* keyboard = keyboardNode->as_table()) {
          warnUnknownKeys(*keyboard, "input.keyboard", {"layout", "variant", "repeat_rate", "repeat_delay"});
          readString(*keyboard, "layout", "input.keyboard.layout", loaded.input.keyboard.layout);
          readString(*keyboard, "variant", "input.keyboard.variant", loaded.input.keyboard.variant);
          readInteger(
              *keyboard, "repeat_rate", "input.keyboard.repeat_rate", 0, 1000, loaded.input.keyboard.repeatRate
          );
          readInteger(
              *keyboard, "repeat_delay", "input.keyboard.repeat_delay", 0, 10000, loaded.input.keyboard.repeatDelay
          );
          validateKeyboardInput(loaded.input.keyboard, keyboardNode->source());
        } else {
          warnAt(keyboardNode->source(), "ignoring input.keyboard (expected table)");
        }
      }

      if (const toml::node* touchpadNode = input->get("touchpad")) {
        if (const auto* touchpad = touchpadNode->as_table()) {
          warnUnknownKeys(*touchpad, "input.touchpad", {"tap", "natural_scroll"});
          readBoolean(*touchpad, "tap", "input.touchpad.tap", loaded.input.touchpad.tap);
          readBoolean(
              *touchpad, "natural_scroll", "input.touchpad.natural_scroll", loaded.input.touchpad.naturalScroll
          );
        } else {
          warnAt(touchpadNode->source(), "ignoring input.touchpad (expected table)");
        }
      }

      if (const toml::node* mouseNode = input->get("mouse")) {
        if (const auto* mouse = mouseNode->as_table()) {
          warnUnknownKeys(*mouse, "input.mouse", {"natural_scroll", "scroll_wheel_step"});
          readBoolean(*mouse, "natural_scroll", "input.mouse.natural_scroll", loaded.input.mouse.naturalScroll);
          readInteger(
              *mouse, "scroll_wheel_step", "input.mouse.scroll_wheel_step", 1, 1000, loaded.input.mouse.scrollWheelStep
          );
        } else {
          warnAt(mouseNode->source(), "ignoring input.mouse (expected table)");
        }
      }

      if (const toml::node* cursorNode = input->get("cursor")) {
        if (const auto* cursor = cursorNode->as_table()) {
          warnUnknownKeys(*cursor, "input.cursor", {"theme", "size"});
          readString(*cursor, "theme", "input.cursor.theme", loaded.input.cursor.theme);
          readInteger(*cursor, "size", "input.cursor.size", 1, 512, loaded.input.cursor.size);
        } else {
          warnAt(cursorNode->source(), "ignoring input.cursor (expected table)");
        }
      }

      if (const toml::node* focusNode = input->get("focus")) {
        if (const auto* focus = focusNode->as_table()) {
          warnUnknownKeys(*focus, "input.focus", {"follows_mouse", "follows_mouse_max_scroll"});
          if (const toml::node* followsMouse = focus->get("follows_mouse")) {
            if (const auto value = followsMouse->value<bool>()) {
              loaded.input.focus.followsMouse = *value;
            } else {
              warnAt(followsMouse->source(), "ignoring input.focus.follows_mouse (expected boolean)");
            }
          }
          if (const toml::node* maxScroll = focus->get("follows_mouse_max_scroll")) {
            if (const auto value = maxScroll->value<double>(); value && !std::isnan(*value)) {
              loaded.input.focus.followsMouseMaxScroll = std::clamp(*value, 0.0, 1.0);
            } else {
              warnAt(maxScroll->source(), "ignoring input.focus.follows_mouse_max_scroll (expected number 0.0-1.0)");
            }
          }
        } else {
          warnAt(focusNode->source(), "ignoring input.focus (expected table)");
        }
      }
    }

    void readOutputs(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("output");
      if (node == nullptr) {
        return;
      }
      const auto* outputs = node->as_table();
      if (outputs == nullptr) {
        warnAt(node->source(), "ignoring output (expected table)");
        return;
      }

      for (const auto& [key, entry] : *outputs) {
        const std::string name(key.str());
        const auto* section = entry.as_table();
        if (section == nullptr) {
          warnAt(entry.source(), "ignoring output.{} (expected table)", name);
          continue;
        }
        warnUnknownKeys(*section, std::string("output.") + name, {"mode", "position", "scale", "transform"});

        if (std::ranges::any_of(loaded.outputs, [&](const OutputRule& rule) { return rule.name == name; })) {
          warnAt(key.source(), "duplicate output section '{}'", name);
          std::erase_if(loaded.outputs, [&](const OutputRule& rule) { return rule.name == name; });
        }
        OutputRule rule;
        rule.name = name;

        if (const toml::node* modeNode = section->get("mode")) {
          const auto value = modeNode->value<std::string>();
          OutputMode mode;
          if (!value || !parseOutputMode(*value, mode)) {
            warnAt(
                modeNode->source(), R"(ignoring output.{}.mode (expected "WIDTHxHEIGHT" or "WIDTHxHEIGHT@HZ"))", name
            );
          } else {
            rule.mode = mode;
          }
        }

        if (const toml::node* positionNode = section->get("position")) {
          const auto* position = positionNode->as_array();
          bool valid = position != nullptr && position->size() == 2;
          std::array<int, 2> parsed{};
          if (valid) {
            for (size_t index = 0; index < parsed.size(); ++index) {
              const auto value = (*position)[index].value<std::int64_t>();
              if (!value) {
                valid = false;
                break;
              }
              parsed[index] = static_cast<int>(
                  std::clamp(*value, static_cast<std::int64_t>(-100000), static_cast<std::int64_t>(100000))
              );
            }
          }
          if (!valid) {
            warnAt(positionNode->source(), "ignoring output.{}.position (expected [x, y] integers)", name);
          } else {
            rule.position = parsed;
          }
        }

        if (const toml::node* scaleNode = section->get("scale")) {
          const auto value = scaleNode->value<double>();
          if (!value || std::isnan(*value)) {
            warnAt(scaleNode->source(), "ignoring output.{}.scale (expected number)", name);
          } else {
            double scale = *value;
            readDouble(*section, "scale", std::string("output.") + name + ".scale", 0.25, 4.0, scale);
            rule.scale = scale;
          }
        }

        if (const toml::node* transformNode = section->get("transform")) {
          const auto value = transformNode->value<std::string>();
          static constexpr std::pair<std::string_view, int> transforms[] = {
              {"normal", 0},  {"90", 1},         {"180", 2},         {"270", 3},
              {"flipped", 4}, {"flipped-90", 5}, {"flipped-180", 6}, {"flipped-270", 7},
          };
          const auto match = value
              ? std::ranges::find_if(transforms, [&](const auto& candidate) { return candidate.first == *value; })
              : std::end(transforms);
          if (match == std::end(transforms)) {
            warnAt(
                transformNode->source(),
                "ignoring output.{}.transform (expected "
                "normal|90|180|270|flipped|flipped-90|flipped-180|flipped-270)",
                name
            );
          } else {
            rule.transform = match->second;
          }
        }

        loaded.outputs.push_back(std::move(rule));
      }
    }

    void readKeybinds(const toml::table& table, Config& loaded) {
      loaded.keybinds = defaultKeybinds();
      const toml::node* node = table.get("keybinds");
      if (node == nullptr) {
        return;
      }
      const auto* section = node->as_table();
      if (section == nullptr) {
        warnAt(node->source(), "ignoring keybinds (expected table)");
        return;
      }

      std::vector<Keybind> configured;
      auto sameChord = [](const Keybind& left, const Keybind& right) {
        return left.modifiers == right.modifiers
            && left.useMod == right.useMod
            && left.keysym == right.keysym
            && left.wheel == right.wheel;
      };
      for (const auto& [key, entry] : *section) {
        const std::string chord(key.str());
        std::string actionStr;
        bool repeatBind = true;

        if (const auto* tbl = entry.as_table()) {
          warnUnknownKeys(*tbl, "keybinds." + chord, {"action", "repeat"});
          const toml::node* actionNode = tbl->get("action");
          if (actionNode == nullptr) {
            warnAt(entry.source(), "ignoring keybind '{}' (table needs an 'action' string)", chord);
            continue;
          }
          const auto actionVal = actionNode->value<std::string>();
          if (!actionVal) {
            warnAt(entry.source(), "ignoring keybind '{}' (table needs an 'action' string)", chord);
            continue;
          }
          actionStr = *actionVal;
          if (const toml::node* repeatNode = tbl->get("repeat")) {
            const auto repeatVal = repeatNode->value<bool>();
            if (repeatVal) {
              repeatBind = *repeatVal;
            } else {
              warnAt(repeatNode->source(), "ignoring keybinds.{}.repeat (expected boolean)", chord);
            }
          }
        } else {
          const auto value = entry.value<std::string>();
          if (!value) {
            warnAt(entry.source(), "ignoring keybind '{}' (expected string or table)", chord);
            continue;
          }
          actionStr = *value;
        }

        Keybind binding;
        if (!parseChord(chord, binding)) {
          if (binding.keysym != XKB_KEY_NoSymbol && binding.modifiers == 0 && !binding.useMod) {
            warnAt(key.source(), "ignoring keybind '{}' (needs at least one modifier)", chord);
          } else {
            warnAt(key.source(), "ignoring keybind '{}' (bad chord)", chord);
          }
          continue;
        }
        binding.repeat = repeatBind;
        if (!parseAction(actionStr, binding)) {
          warnAt(key.source(), "ignoring keybind '{}' (unknown action '{}')", chord, actionStr);
          continue;
        }

        if (std::ranges::any_of(configured, [&](const Keybind& existing) { return sameChord(existing, binding); })) {
          warnAt(key.source(), "duplicate keybind {}", chord);
        }
        std::erase_if(configured, [&](const Keybind& existing) { return sameChord(existing, binding); });
        configured.push_back(binding);
        std::erase_if(loaded.keybinds, [&](const Keybind& existing) { return sameChord(existing, binding); });
        if (binding.action != KeybindAction::None) {
          loaded.keybinds.push_back(std::move(binding));
        }
      }
    }

    void readWindowRules(const toml::table& table, Config& loaded) {
      const toml::node* node = table.get("rule");
      if (node == nullptr) {
        return;
      }
      const auto* rules = node->as_array();
      if (rules == nullptr) {
        warnAt(node->source(), "ignoring rule (expected [[rule]] array of tables)");
        return;
      }

      for (const auto& entry : *rules) {
        const auto* section = entry.as_table();
        if (section == nullptr) {
          warnAt(entry.source(), "ignoring rule entry (expected table)");
          continue;
        }
        warnUnknownKeys(
            *section, "rule",
            {"match", "default_floating", "default_size", "default_width", "default_workspace", "default_fullscreen",
             "opacity"}
        );

        WindowRule rule;

        if (const toml::node* matchNode = section->get("match")) {
          if (const auto* match = matchNode->as_table()) {
            warnUnknownKeys(*match, "rule.match", {"app_id", "title"});
            if (const toml::node* appIdNode = match->get("app_id")) {
              if (const auto value = appIdNode->value<std::string>()) {
                rule.appIdPattern = *value;
                try {
                  rule.appIdRegex = std::regex(rule.appIdPattern);
                } catch (const std::regex_error& error) {
                  warnAt(appIdNode->source(), "invalid regex in rule.match.app_id: {}", error.what());
                  continue;
                }
              } else {
                warnAt(appIdNode->source(), "ignoring rule.match.app_id (expected string)");
              }
            }
            if (const toml::node* titleNode = match->get("title")) {
              if (const auto value = titleNode->value<std::string>()) {
                rule.titlePattern = *value;
                try {
                  rule.titleRegex = std::regex(rule.titlePattern);
                } catch (const std::regex_error& error) {
                  warnAt(titleNode->source(), "invalid regex in rule.match.title: {}", error.what());
                  continue;
                }
              } else {
                warnAt(titleNode->source(), "ignoring rule.match.title (expected string)");
              }
            }
          } else {
            warnAt(matchNode->source(), "ignoring rule.match (expected table)");
          }
        }

        if (const toml::node* n = section->get("default_floating")) {
          if (n->is_boolean()) {
            rule.defaultFloating = n->value<bool>();
          } else {
            warnAt(n->source(), "ignoring rule.default_floating (expected boolean)");
          }
        }

        if (const toml::node* n = section->get("default_size")) {
          const auto* arr = n->as_array();
          bool valid = arr != nullptr && arr->size() == 2;
          std::array<int, 2> parsed{};
          if (valid) {
            for (size_t index = 0; index < 2; ++index) {
              const auto value = (*arr)[index].value<std::int64_t>();
              if (!value || *value < 1 || *value > 100000) {
                valid = false;
                break;
              }
              parsed[index] = static_cast<int>(*value);
            }
          }
          if (!valid) {
            warnAt(n->source(), "ignoring rule.default_size (expected [width, height] positive integers)");
          } else {
            rule.defaultSize = parsed;
          }
        }

        if (const toml::node* n = section->get("default_width")) {
          const auto value = n->value<double>();
          if (!value || std::isnan(*value)) {
            warnAt(n->source(), "ignoring rule.default_width (expected number 0.1-1.0)");
          } else {
            const double used = std::clamp(*value, 0.1, 1.0);
            if (used != *value) {
              warnAt(n->source(), "rule.default_width = {} out of range, clamped to {}", *value, used);
            }
            rule.defaultWidth = used;
          }
        }

        if (const toml::node* n = section->get("default_workspace")) {
          const auto value = n->value<std::int64_t>();
          if (!value || *value < 1 || *value > 9) {
            warnAt(n->source(), "ignoring rule.default_workspace (expected integer 1-9)");
          } else {
            rule.defaultWorkspace = static_cast<int>(*value);
          }
        }

        if (const toml::node* n = section->get("default_fullscreen")) {
          if (n->is_boolean()) {
            rule.defaultFullscreen = n->value<bool>();
          } else {
            warnAt(n->source(), "ignoring rule.default_fullscreen (expected boolean)");
          }
        }

        if (const toml::node* n = section->get("opacity")) {
          const auto value = n->value<double>();
          if (!value || std::isnan(*value)) {
            warnAt(n->source(), "ignoring rule.opacity (expected number 0.0-1.0)");
          } else {
            const double used = std::clamp(*value, 0.0, 1.0);
            if (used != *value) {
              warnAt(n->source(), "rule.opacity = {} out of range, clamped to {}", *value, used);
            }
            rule.opacity = used;
          }
        }

        loaded.windowRules.push_back(std::move(rule));
      }
    }

    void warnUnknownTopLevel(const toml::table& table) {
      for (const auto& [key, value] : table) {
        (void)value;
        if (!knownKey(
                key.str(),
                {"appearance", "layout", "general", "input", "keybinds", "output", "rule", "workspace", "include"}
            )) {
          warnAt(key.source(), "unknown key {}", key.str());
        }
      }
    }
    bool parseInto(Config& out) {
      g_diagnostics.clear();
      g_watchPaths.clear();
      g_watchPaths.push_back(g_rootPath);

      std::error_code error;
      if (!std::filesystem::is_regular_file(g_rootPath, error) || error) {
        return false;
      }

      try {
        auto result = configmerge::mergeWithIncludes(g_rootPath);
        g_diagnostics.insert(
            g_diagnostics.end(), std::make_move_iterator(result.diagnostics.begin()),
            std::make_move_iterator(result.diagnostics.end())
        );
        for (const auto& path : result.loadedFiles) {
          if (std::ranges::find(g_watchPaths, path) == g_watchPaths.end()) {
            g_watchPaths.push_back(path);
          }
        }
        if (result.hadParseError) {
          return false;
        }

        Config loaded;
        loaded.keybinds = defaultKeybinds();
        warnUnknownTopLevel(result.merged);
        readAppearance(result.merged, loaded);
        readLayout(result.merged, loaded);
        readGeneral(result.merged, loaded);
        readInput(result.merged, loaded);
        readOutputs(result.merged, loaded);
        readKeybinds(result.merged, loaded);
        readWindowRules(result.merged, loaded);
        readWorkspaces(result.merged, loaded);

        // Reject config if any error-level diagnostics were emitted.
        const bool hasErrors = std::ranges::any_of(g_diagnostics, [](const ConfigDiagnostic& d) {
          return d.severity == ConfigDiagnostic::Severity::Error;
        });
        if (hasErrors) {
          return false;
        }

        out = std::move(loaded);
        return true;
      } catch (const std::exception& exception) {
        emitDiag(ConfigDiagnostic::Severity::Error, nullptr, std::format("config load error: {}", exception.what()));
      } catch (...) {
        emitDiag(ConfigDiagnostic::Severity::Error, nullptr, "config load error: unknown error");
      }
      return false;
    }

  } // namespace

  const Config& config() { return g_config; }

  const std::vector<ConfigDiagnostic>& configDiagnostics() { return g_diagnostics; }

  const std::filesystem::path& configRootPath() { return g_rootPath; }

  void loadConfig(const char* explicitPath) {
    g_rootPath = explicitPath == nullptr ? defaultConfigPath() : std::filesystem::path(explicitPath);
    g_explicitPath = explicitPath != nullptr;

    Config loaded;
    loaded.keybinds = defaultKeybinds();
    if (!parseInto(loaded)) {
      std::error_code error;
      if (!std::filesystem::is_regular_file(g_rootPath, error) || error) {
        if (g_explicitPath) {
          emitDiag(
              ConfigDiagnostic::Severity::Error, nullptr, std::format("config file not found: {}", g_rootPath.string())
          );
        } else {
          kLog.info("no config file found: {}, using defaults", g_rootPath.string());
        }
      }
    }
    g_config = std::move(loaded);
  }

  bool reloadConfig() {
    Config loaded;
    loaded.keybinds = defaultKeybinds();
    if (parseInto(loaded)) {
      g_config = std::move(loaded);
      return true;
    }
    kLog.warn("config reload failed; keeping previous configuration");
    return false;
  }

  const std::vector<std::filesystem::path>& configWatchPaths() { return g_watchPaths; }

  ResolvedWindowRule resolveWindowRules(const char* appId, const char* title) {
    ResolvedWindowRule resolved;
    const std::string_view appIdView = appId != nullptr ? appId : "";
    const std::string_view titleView = title != nullptr ? title : "";

    for (const auto& rule : g_config.windowRules) {
      if (!rule.appIdPattern.empty()) {
        if (appIdView.empty() || !std::regex_search(appIdView.begin(), appIdView.end(), rule.appIdRegex)) {
          continue;
        }
      }
      if (!rule.titlePattern.empty()) {
        if (titleView.empty() || !std::regex_search(titleView.begin(), titleView.end(), rule.titleRegex)) {
          continue;
        }
      }
      // Last writer wins: overwrite each field the rule sets.
      if (rule.defaultFloating) {
        resolved.defaultFloating = rule.defaultFloating;
      }
      if (rule.defaultSize) {
        resolved.defaultSize = rule.defaultSize;
      }
      if (rule.defaultWidth) {
        resolved.defaultWidth = rule.defaultWidth;
      }
      if (rule.defaultWorkspace) {
        resolved.defaultWorkspace = rule.defaultWorkspace;
      }
      if (rule.defaultFullscreen) {
        resolved.defaultFullscreen = rule.defaultFullscreen;
      }
      if (rule.opacity) {
        resolved.opacity = rule.opacity;
      }
    }
    return resolved;
  }

  bool anyWindowRuleHasTitlePattern() {
    return std::ranges::any_of(g_config.windowRules, [](const WindowRule& rule) { return !rule.titlePattern.empty(); });
  }

  ResolvedLayoutConfig resolveGlobalLayout() {
    const auto& cfg = g_config;
    ResolvedLayoutConfig r;
    r.mode = cfg.layout.mode;
    r.gap = cfg.layout.gap;
    r.defaultWidthFraction = cfg.layout.defaultWidthFraction;
    r.widthPresets = cfg.layout.widthPresets;
    const int borderWidth = cfg.appearance.totalBorderWidth();
    r.totalGap = r.gap + 2 * borderWidth;
    r.edgePad = r.gap + borderWidth;
    return r;
  }

  std::vector<ResolvedWorkspace> resolveWorkspacesForOutput(const char* outputName) {
    const auto& cfg = g_config;
    const std::string_view outName = outputName != nullptr ? outputName : "";

    // Partition workspace entries into base and output-qualified for this output.
    std::vector<const WorkspaceConfig*> baseEntries;
    std::vector<const WorkspaceConfig*> outputEntries;
    for (const auto& ws : cfg.workspaces) {
      if (ws.output.empty()) {
        baseEntries.push_back(&ws);
      } else if (ws.output == outName) {
        outputEntries.push_back(&ws);
      }
    }

    // Helper: resolve a WorkspaceLayoutOverrides on top of an existing ResolvedLayoutConfig.
    auto applyOverrides = [&](ResolvedLayoutConfig& resolved, const WorkspaceLayoutOverrides& overrides) {
      if (overrides.mode) {
        resolved.mode = *overrides.mode;
      }
      if (overrides.gap) {
        resolved.gap = *overrides.gap;
      }
      if (overrides.defaultWidthFraction) {
        resolved.defaultWidthFraction = *overrides.defaultWidthFraction;
      }
      if (overrides.widthPresets) {
        resolved.widthPresets = *overrides.widthPresets;
      }
      const int borderWidth = cfg.appearance.totalBorderWidth();
      resolved.totalGap = resolved.gap + 2 * borderWidth;
      resolved.edgePad = resolved.gap + borderWidth;
    };

    // Start with base entries.
    struct Slot {
      std::string name;
      ResolvedLayoutConfig layout;
    };
    std::vector<Slot> slots;
    slots.reserve(baseEntries.size());
    for (size_t i = 0; i < baseEntries.size(); ++i) {
      const auto& base = *baseEntries[i];
      Slot slot;
      slot.name = base.name.empty() ? std::to_string(i + 1) : base.name;
      slot.layout = resolveGlobalLayout();
      applyOverrides(slot.layout, base.layout);
      slots.push_back(std::move(slot));
    }

    // Apply output-qualified entries.
    for (const auto* entry : outputEntries) {
      // Find target slot.
      int targetIdx = -1;
      if (entry->index.has_value()) {
        // index selector (1-based).
        const int idx = *entry->index - 1;
        if (idx >= 0 && idx < static_cast<int>(slots.size())) {
          targetIdx = idx;
        }
        // Invalid index was already warned at parse time; skip.
      } else if (!entry->append && !entry->name.empty()) {
        // name selector: find matching base slot.
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
          // Match against the original base entry name, not the display name.
          if (i < static_cast<int>(baseEntries.size()) && baseEntries[static_cast<size_t>(i)]->name == entry->name) {
            targetIdx = i;
            break;
          }
        }
        // Unmatched name was already warned at parse time; skip.
      }

      if (targetIdx >= 0) {
        // Overlay: apply per-field on top of the base slot.
        Slot& slot = slots[static_cast<size_t>(targetIdx)];
        applyOverrides(slot.layout, entry->layout);
        // Name override (rename) when index is the selector.
        if (entry->index.has_value() && !entry->name.empty()) {
          slot.name = entry->name;
        }
      } else if (entry->append) {
        // Append new workspace.
        Slot slot;
        slot.name = entry->name.empty() ? std::to_string(slots.size() + 1) : entry->name;
        slot.layout = resolveGlobalLayout();
        applyOverrides(slot.layout, entry->layout);
        slots.push_back(std::move(slot));
      }
    }

    // Convert to result.
    std::vector<ResolvedWorkspace> result;
    result.reserve(slots.size());
    for (auto& slot : slots) {
      result.push_back({std::move(slot.name), std::move(slot.layout)});
    }
    return result;
  }

} // namespace umbriel
