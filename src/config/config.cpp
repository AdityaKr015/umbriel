#include "config/config.h"

#include "config/config_diag.h"
#include "config/config_merge.h"
#include "config/keybind_parse.h"
#include "config/resolve.h"
#include "config/section.h"
#include "config/store.h"
#include "config/value_parse.h"
#include "core/log.h"

// clang-format off
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <string_view>
#include <utility>

namespace umbriel {

  namespace {

    constexpr Logger kLog("config");

    // Well past any real layout: a value this large already means "no limit".
    constexpr double kMaxFollowsMouseScroll = 100.0;

    std::string lowercase(std::string_view text) {
      std::string lowered(text);
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return lowered;
    }

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
      configStore().addDiagnostic(std::move(diag));
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

    std::optional<LayoutMode> readLayoutMode(Section& section, std::string_view context) {
      const toml::node* node = section.take("mode");
      if (node == nullptr) {
        return std::nullopt;
      }
      const auto* value = node->as_string();
      if (value == nullptr) {
        warnAt(node->source(), R"({}.mode must be a string ("scrolling" or "dwindle"))", context);
        return std::nullopt;
      }
      const std::string_view mode = value->get();
      if (mode == "dwindle") {
        return LayoutMode::Dwindle;
      }
      if (mode == "scrolling") {
        return LayoutMode::Scrolling;
      }
      warnAt(node->source(), R"(unknown {}.mode "{}" (expected "scrolling" or "dwindle"))", context, mode);
      return std::nullopt;
    }

    std::optional<std::vector<double>> readWidthPresets(Section& section, std::string_view context) {
      const toml::node* node = section.take("width_presets");
      if (node == nullptr) {
        return std::nullopt;
      }
      const auto* array = node->as_array();
      if (array == nullptr || array->empty()) {
        warnAt(node->source(), "ignoring {}.width_presets (expected non-empty array of numbers)", context);
        return std::nullopt;
      }

      std::vector<double> parsed;
      parsed.reserve(array->size());
      for (const auto& entry : *array) {
        const auto value = entry.value<double>();
        if (!value || std::isnan(*value)) {
          warnAt(node->source(), "ignoring {}.width_presets (expected non-empty array of numbers)", context);
          return std::nullopt;
        }
        const double used = std::clamp(*value, 0.1, 1.0);
        if (used != *value) {
          warnAt(entry.source(), "{}.width_presets = {} out of range, clamped to {}", context, *value, used);
        }
        parsed.push_back(used);
      }
      return parsed;
    }

    void readWorkspaceLayoutOverrides(
        const toml::table& section, std::string_view context, WorkspaceLayoutOverrides& overrides
    ) {
      const std::string layoutContext = std::string(context) + ".layout";
      readSection(
          section, "layout", configStore().mutableDiagnostics(),
          [&](Section& s) {
            if (const auto mode = readLayoutMode(s, layoutContext)) {
              overrides.mode = mode;
            }
            s.integer("gap", 0, 500, overrides.gap);
            if (auto presets = readWidthPresets(s, layoutContext)) {
              overrides.widthPresets = std::move(*presets);
            }
            s.sub("scrolling", [&](Section& sc) {
              sc.real("default_width_fraction", 0.1, 1.0, overrides.scrolling.defaultWidthFraction)
                  .boolean("center_underfull_strip", overrides.scrolling.centerUnderfullStrip);
            });
          },
          layoutContext
      );
    }

    std::vector<std::string> numericWorkspaceNames(size_t count) {
      std::vector<std::string> names;
      names.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        names.push_back(std::to_string(i + 1));
      }
      return names;
    }

    WorkspaceConfig parseWorkspaceEntry(const toml::table& section, std::string_view context) {
      WorkspaceConfig ws;
      Section keys(section, std::string(context), configStore().mutableDiagnostics());
      // `layout` is read by readWorkspaceLayoutOverrides below, which takes the
      // raw table rather than this reader.
      keys.custom("layout");

      if (const toml::node* nameNode = keys.take("name")) {
        if (const auto value = nameNode->value<std::string>()) {
          if (value->empty()) {
            errorAt(nameNode->source(), "{}.name must not be empty", context);
          } else {
            ws.name = *value;
          }
        } else {
          errorAt(nameNode->source(), "{}.name must be a string", context);
        }
      }
      if (const toml::node* outputNode = keys.take("output")) {
        if (const auto value = outputNode->value<std::string>()) {
          if (value->empty()) {
            errorAt(outputNode->source(), "{}.output must not be empty", context);
          } else {
            ws.output = *value;
          }
        } else {
          errorAt(outputNode->source(), "{}.output must be a string", context);
        }
      }
      if (const toml::node* indexNode = keys.take("index")) {
        const auto value = indexNode->value<std::int64_t>();
        if (!value || *value < 1 || *value > static_cast<std::int64_t>(kMaxWorkspaces)) {
          errorAt(indexNode->source(), "{}.index must be an integer from 1 to {}", context, kMaxWorkspaces);
        } else {
          ws.index = static_cast<int>(*value);
        }
      }

      readWorkspaceLayoutOverrides(section, context, ws.layout);
      return ws;
    }

    void readWorkspaces(Section& root, Config& loaded) {
      const toml::node* node = root.take("workspace");
      if (node == nullptr) {
        return;
      }
      const auto* workspaces = node->as_array();
      if (workspaces == nullptr) {
        errorAt(node->source(), "workspace must be a [[workspace]] array of tables");
        return;
      }

      struct ParsedEntry {
        WorkspaceConfig ws;
        toml::source_region source;
        int arrayIndex;
      };
      std::vector<ParsedEntry> entries;
      entries.reserve(workspaces->size());

      int entryIndex = 0;
      for (const auto& entry : *workspaces) {
        const auto* section = entry.as_table();
        if (section == nullptr) {
          errorAt(entry.source(), "workspace[{}] must be a table", entryIndex);
          ++entryIndex;
          continue;
        }

        const std::string context = std::format("workspace[{}]", entryIndex);
        WorkspaceConfig ws = parseWorkspaceEntry(*section, context);
        const bool hasName = !ws.name.empty();
        const bool hasIndex = ws.index.has_value();
        if (hasName == hasIndex) {
          errorAt(entry.source(), "{} must set exactly one of name or index", context);
        }

        entries.push_back({std::move(ws), entry.source(), entryIndex});
        ++entryIndex;
      }

      const auto sameSelector = [](const WorkspaceConfig& left, const WorkspaceConfig& right) {
        if (left.output != right.output || left.index.has_value() != right.index.has_value()) {
          return false;
        }
        return left.index ? left.index == right.index : left.name == right.name;
      };

      for (size_t i = 0; i < entries.size(); ++i) {
        const auto& current = entries[i];
        const auto& ws = current.ws;
        const std::string context = std::format("workspace[{}]", current.arrayIndex);
        if (ws.name.empty() != ws.index.has_value()) {
          continue;
        }

        for (size_t j = 0; j < i; ++j) {
          if (sameSelector(entries[j].ws, ws)) {
            errorAt(current.source, "{} duplicates workspace rule {}", context, entries[j].arrayIndex);
            break;
          }
        }

        const bool targetExists = workspaceRuleTargetExists(loaded, ws);
        if (!targetExists) {
          const std::string selector =
              ws.index ? std::format("index {}", *ws.index) : std::format("name '{}'", ws.name);
          if (ws.output.empty()) {
            errorAt(current.source, "{}: {} does not match any workspace inventory", context, selector);
          } else {
            errorAt(current.source, "{}: {} does not exist on output '{}'", context, selector, ws.output);
          }
        }
      }

      for (auto& entry : entries) {
        loaded.workspaceRules.push_back(std::move(entry.ws));
      }
    }

    void readColors(Section& root, Config& loaded) {
      auto& colors = loaded.colors;
      root.sub("colors", [&](Section& s) {
        s.color("background", colors.background)
            .color("text_primary", colors.textPrimary)
            .color("text_muted", colors.textMuted)
            .color("accent_primary", colors.accentPrimary)
            .color("accent_secondary", colors.accentSecondary)
            .color("warning", colors.warning)
            .color("error", colors.error);
      });
    }

    void readAppearance(Section& root, Config& loaded) {
      auto& a = loaded.appearance;
      root.sub("appearance", [&](Section& s) {
        s.integer("border_width", 0, 100, a.borderWidth)
            .integer("outer_border_width", 0, 100, a.outerBorderWidth)
            .integer("corner_radius", 0, 500, a.cornerRadius)
            .color("border_focused", a.borderFocused)
            .color("border_unfocused", a.borderUnfocused)
            .color("scratchpad_border_focused", a.scratchpadBorderFocused)
            .color("scratchpad_border_unfocused", a.scratchpadBorderUnfocused)
            .color("outer_border_color", a.outerBorderColor)
            .color("insert_hint_color", a.insertHintColor)
            .color("backdrop_color", a.backdropColor)
            .integer("animation_ms", 1, 10000, a.animationMs)
            .boolean("prefer_no_csd", a.preferNoCsd);
        s.sub("blur", [&](Section& blur) {
          blur.boolean("enabled", a.blur.enabled)
              .boolean("optimized", a.blur.optimized)
              .integer("passes", 0, 8, a.blur.passes)
              .integer("radius", 0, 100, a.blur.radius)
              .real("noise", 0.0, 1.0, a.blur.noise)
              .real("brightness", 0.0, 2.0, a.blur.brightness)
              .real("contrast", 0.0, 2.0, a.blur.contrast)
              .real("saturation", 0.0, 2.0, a.blur.saturation);
        });
        s.sub("shadow", [&](Section& shadow) {
          shadow.boolean("enabled", a.shadow.enabled)
              .integer("softness", 0, 200, a.shadow.softness)
              .integer("offset_x", -200, 200, a.shadow.offsetX)
              .integer("offset_y", -200, 200, a.shadow.offsetY)
              .color("color", a.shadow.color);
        });
      });
    }

    void readOverview(Section& root, Config& loaded) {
      root.sub("overview", [&](Section& s) {
        s.real("zoom", 0.1, 0.75, loaded.overview.zoom)
            .color("background_tint", loaded.overview.backgroundTint)
            .color("workspace_background", loaded.overview.workspaceBackground);
      });
    }

    void readLayout(Section& root, Config& loaded) {
      root.sub("layout", [&](Section& s) {
        if (const auto mode = readLayoutMode(s, "layout")) {
          loaded.layout.mode = *mode;
        }
        s.integer("gap", 0, 500, loaded.layout.gap);
        if (auto presets = readWidthPresets(s, "layout")) {
          loaded.layout.widthPresets = std::move(*presets);
        }
        s.sub("scrolling", [&](Section& sc) {
          sc.real("default_width_fraction", 0.1, 1.0, loaded.layout.scrolling.defaultWidthFraction)
              .boolean("center_underfull_strip", loaded.layout.scrolling.centerUnderfullStrip);
        });
      });
    }

    void readWorkspaceSettings(Section& root, Config& loaded) {
      root.sub("workspaces", [&](Section& s) { s.boolean("back_and_forth", loaded.workspaces.backAndForth); });
    }

    void readGeneral(Section& root, Config& loaded) {
      root.sub("general", [&](Section& s) {
        if (const toml::node* node = s.take("mod_key")) {
          const auto value = node->value<std::string>();
          if (!value) {
            warnAt(node->source(), "general.mod_key must be a string");
          } else {
            const std::string modifier = lowercase(*value);
            if (modifier == "super" || modifier == "logo" || modifier == "win") {
              loaded.general.modKey = ModifierKey::Super;
            } else if (modifier == "alt") {
              loaded.general.modKey = ModifierKey::Alt;
            } else if (modifier == "ctrl" || modifier == "control") {
              loaded.general.modKey = ModifierKey::Control;
            } else if (modifier == "shift") {
              loaded.general.modKey = ModifierKey::Shift;
            } else {
              warnAt(
                  node->source(), R"(unknown general.mod_key "{}" (expected "Super", "Alt", "Ctrl", or "Shift"))",
                  *value
              );
            }
          }
        }
        s.boolean("xwayland", loaded.general.xwayland)
            .boolean("show_cheatsheet", loaded.general.showCheatsheet)
            .boolean("focus_on_activate", loaded.general.focusOnActivate)
            .strings("autostart", loaded.general.autostart);
      });
    }

    void readEnvironment(Section& root, Config& loaded) {
      root.sub("environment", [&](Section& s) { s.eachString(loaded.environment.variables); });
    }

    bool validateKeyboardInput(
        const Config::Input::Keyboard& keyboard, const toml::source_region& source, std::string_view context
    ) {
      if (keyboard.layout.empty() && keyboard.variant.empty() && keyboard.options.empty()) {
        return true;
      }
      xkb_context* xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (xkbContext == nullptr) {
        warnAt(source, "unable to validate {} XKB configuration", context);
        return false;
      }
      const xkb_rule_names names{
          .rules = nullptr,
          .model = nullptr,
          .layout = keyboard.layout.empty() ? nullptr : keyboard.layout.c_str(),
          .variant = keyboard.variant.empty() ? nullptr : keyboard.variant.c_str(),
          .options = keyboard.options.empty() ? nullptr : keyboard.options.c_str(),
      };
      xkb_keymap* keymap = xkb_keymap_new_from_names(xkbContext, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (keymap == nullptr) {
        warnAt(
            source, "ignoring {} layout='{}' variant='{}' options='{}' (invalid XKB configuration)", context,
            keyboard.layout, keyboard.variant, keyboard.options
        );
        xkb_context_unref(xkbContext);
        return false;
      }
      xkb_keymap_unref(keymap);
      xkb_context_unref(xkbContext);
      return true;
    }

    void readOptionalText(
        Section& section, std::string_view key, std::optional<std::string>& target, std::string_view context
    ) {
      const toml::node* node = section.take(key);
      if (node == nullptr) {
        return;
      }
      if (const auto value = node->value<std::string>()) {
        target = *value;
      } else {
        warnAt(node->source(), "ignoring {}.{} (expected string)", context, key);
      }
    }

    void readInputDevices(Section& input, Config::Input& configured) {
      const toml::node* node = input.take("device");
      if (node == nullptr) {
        return;
      }
      const auto* devices = node->as_array();
      if (devices == nullptr) {
        errorAt(node->source(), "input.device must be a [[input.device]] array of tables");
        return;
      }

      size_t index = 0;
      for (const auto& entry : *devices) {
        const std::string context = std::format("input.device[{}]", index++);
        const auto* table = entry.as_table();
        if (table == nullptr) {
          errorAt(entry.source(), "{} must be a table", context);
          continue;
        }

        Config::Input::Device device;
        Section keys(*table, context, configStore().mutableDiagnostics());
        bool validName = false;
        if (const toml::node* nameNode = keys.take("name")) {
          if (const auto name = nameNode->value<std::string>(); name && !name->empty()) {
            device.name = *name;
            validName = true;
          } else {
            errorAt(nameNode->source(), "{}.name must be a non-empty string", context);
          }
        } else {
          errorAt(entry.source(), "{} must set name", context);
        }

        readOptionalText(keys, "layout", device.layout, context);
        readOptionalText(keys, "variant", device.variant, context);
        readOptionalText(keys, "options", device.options, context);
        keys.integer("repeat_rate", 0, 1000, device.repeatRate)
            .integer("repeat_delay", 0, 10000, device.repeatDelay)
            .boolean("tap", device.tap)
            .boolean("natural_scroll", device.naturalScroll);

        if (!validName) {
          continue;
        }
        if (std::ranges::any_of(configured.devices, [&](const Config::Input::Device& existing) {
              return existing.name == device.name;
            })) {
          errorAt(entry.source(), "{} duplicates device '{}'", context, device.name);
          continue;
        }

        if (device.layout || device.variant || device.options) {
          Config::Input::Keyboard keyboard = configured.keyboard;
          if (device.layout) {
            keyboard.layout = *device.layout;
          }
          if (device.variant) {
            keyboard.variant = *device.variant;
          }
          if (device.options) {
            keyboard.options = *device.options;
          }
          if (!validateKeyboardInput(keyboard, entry.source(), context)) {
            device.layout.reset();
            device.variant.reset();
            device.options.reset();
          }
        }
        configured.devices.push_back(std::move(device));
      }
    }

    void readInput(Section& root, Config& loaded) {
      auto& in = loaded.input;
      root.sub("input", [&](Section& s) {
        s.sub("keyboard", [&](Section& k) {
          k.text("layout", in.keyboard.layout)
              .text("variant", in.keyboard.variant)
              .text("options", in.keyboard.options)
              .integer("repeat_rate", 0, 1000, in.keyboard.repeatRate)
              .integer("repeat_delay", 0, 10000, in.keyboard.repeatDelay);
        });
        if (const toml::node* keyboardNode = s.node("keyboard");
            keyboardNode != nullptr && !validateKeyboardInput(in.keyboard, keyboardNode->source(), "input.keyboard")) {
          in.keyboard.layout.clear();
          in.keyboard.variant.clear();
          in.keyboard.options.clear();
        }
        s.sub("touchpad", [&](Section& t) {
          t.boolean("tap", in.touchpad.tap).boolean("natural_scroll", in.touchpad.naturalScroll);
        });
        s.sub("mouse", [&](Section& m) {
          m.boolean("natural_scroll", in.mouse.naturalScroll)
              .integer("scroll_wheel_step", 1, 1000, in.mouse.scrollWheelStep);
        });
        s.sub("cursor", [&](Section& c) { c.text("theme", in.cursor.theme).integer("size", 1, 512, in.cursor.size); });
        s.sub("focus", [&](Section& f) {
          // The limit is measured in viewport widths and the quantity it is
          // compared against is unbounded: revealing a column three screens away
          // is 3.0. The upper bound here is a nonsense-catcher, not a ceiling.
          // Below zero would refuse focus even for a window already fully
          // visible, which disables hover focus rather than limiting it.
          f.boolean("follows_mouse", in.focus.followsMouse)
              .real("follows_mouse_max_scroll", 0.0, kMaxFollowsMouseScroll, in.focus.followsMouseMaxScroll);
        });
        readInputDevices(s, in);
      });
    }

    void readOutputs(Section& root, Config& loaded) {
      const toml::node* node = root.take("output");
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
        Section keys(*section, "output." + name, configStore().mutableDiagnostics());

        if (std::ranges::any_of(loaded.outputs, [&](const OutputRule& rule) { return rule.name == name; })) {
          warnAt(key.source(), "duplicate output section '{}'", name);
          std::erase_if(loaded.outputs, [&](const OutputRule& rule) { return rule.name == name; });
        }
        OutputRule rule;
        rule.name = name;
        if (const toml::node* workspacesNode = keys.take("workspaces")) {
          if (const auto count = workspacesNode->value<std::int64_t>()) {
            if (*count < 1 || *count > static_cast<std::int64_t>(kMaxWorkspaces)) {
              errorAt(
                  workspacesNode->source(), "output.{}.workspaces must be an integer from 1 to {}", name, kMaxWorkspaces
              );
            } else {
              rule.workspaces = numericWorkspaceNames(static_cast<size_t>(*count));
            }
          } else if (const auto* names = workspacesNode->as_array()) {
            bool valid = true;
            if (names->empty() || names->size() > kMaxWorkspaces) {
              errorAt(
                  workspacesNode->source(), "output.{}.workspaces must contain 1 to {} names", name, kMaxWorkspaces
              );
              valid = false;
            }

            std::vector<std::string> parsed;
            parsed.reserve(names->size());
            for (const auto& item : *names) {
              const auto value = item.value<std::string>();
              if (!value || value->empty()) {
                errorAt(item.source(), "output.{}.workspaces entries must be non-empty strings", name);
                valid = false;
                continue;
              }
              if (std::ranges::find(parsed, *value) != parsed.end()) {
                errorAt(item.source(), "output.{}.workspaces contains duplicate name '{}'", name, *value);
                valid = false;
                continue;
              }
              parsed.push_back(*value);
            }
            if (valid) {
              rule.workspaces = std::move(parsed);
            }
          } else if (const auto value = workspacesNode->value<std::string>()) {
            if (*value != "dynamic") {
              errorAt(
                  workspacesNode->source(), R"(output.{}.workspaces must be a count, a name array, or "dynamic")", name
              );
            }
          } else {
            errorAt(
                workspacesNode->source(), R"(output.{}.workspaces must be a count, a name array, or "dynamic")", name
            );
          }
        }

        if (const toml::node* modeNode = keys.take("mode")) {
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

        if (const toml::node* positionNode = keys.take("position")) {
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

        keys.real("scale", 0.25, 4.0, rule.scale);

        if (const toml::node* transformNode = keys.take("transform")) {
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

    void readKeybinds(Section& root, Config& loaded) {
      const toml::node* node = root.take("keybinds");
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
        return left.submap == right.submap
            && left.modifiers == right.modifiers
            && left.useMod == right.useMod
            && left.modifierOnly == right.modifierOnly
            && left.keysym == right.keysym
            && left.wheel == right.wheel
            && left.mouseButton == right.mouseButton;
      };
      for (const auto& [key, entry] : *section) {
        const std::string chord(key.str());
        std::string actionStr;
        bool repeatBind = true;

        if (const auto* tbl = entry.as_table()) {
          Section bind(*tbl, "keybinds." + chord, configStore().mutableDiagnostics());
          // Read `repeat` before validating the action: an entry rejected for a
          // bad action must not also be told its `repeat` key is unknown.
          bind.boolean("repeat", repeatBind);
          const toml::node* actionNode = bind.take("action");
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
        binding.repeat = binding.modifierOnly ? false : repeatBind;
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
        loaded.keybinds.push_back(std::move(binding));
      }
    }

    void readWindowRules(Section& root, Config& loaded) {
      const toml::node* node = root.take("window_rule");
      if (node == nullptr) {
        return;
      }
      const auto* rules = node->as_array();
      if (rules == nullptr) {
        warnAt(node->source(), "ignoring window_rule (expected [[window_rule]] array of tables)");
        return;
      }

      for (const auto& entry : *rules) {
        const auto* section = entry.as_table();
        if (section == nullptr) {
          warnAt(entry.source(), "ignoring window_rule entry (expected table)");
          continue;
        }
        Section keys(*section, "window_rule", configStore().mutableDiagnostics());

        WindowRule rule;

        if (const toml::node* matchNode = keys.take("match")) {
          if (const auto* match = matchNode->as_table()) {
            Section matchKeys(*match, "window_rule.match", configStore().mutableDiagnostics());
            if (const toml::node* appIdNode = matchKeys.take("app_id")) {
              if (const auto value = appIdNode->value<std::string>()) {
                rule.appIdPattern = *value;
                try {
                  rule.appIdRegex = std::regex(rule.appIdPattern);
                } catch (const std::regex_error& error) {
                  warnAt(appIdNode->source(), "invalid regex in window_rule.match.app_id: {}", error.what());
                  continue;
                }
              } else {
                warnAt(appIdNode->source(), "ignoring window_rule.match.app_id (expected string)");
              }
            }
            if (const toml::node* titleNode = matchKeys.take("title")) {
              if (const auto value = titleNode->value<std::string>()) {
                rule.titlePattern = *value;
                try {
                  rule.titleRegex = std::regex(rule.titlePattern);
                } catch (const std::regex_error& error) {
                  warnAt(titleNode->source(), "invalid regex in window_rule.match.title: {}", error.what());
                  continue;
                }
              } else {
                warnAt(titleNode->source(), "ignoring window_rule.match.title (expected string)");
              }
            }
            if (const toml::node* focusedNode = matchKeys.take("is_focused")) {
              if (focusedNode->is_boolean()) {
                rule.matchFocused = focusedNode->value<bool>();
              } else {
                warnAt(focusedNode->source(), "ignoring window_rule.match.is_focused (expected boolean)");
              }
            }
          } else {
            warnAt(matchNode->source(), "ignoring window_rule.match (expected table)");
          }
        }

        keys.boolean("default_floating", rule.defaultFloating)
            .boolean("default_fullscreen", rule.defaultFullscreen)
            .boolean("default_maximize", rule.defaultMaximize)
            .boolean("focus_on_activate", rule.focusOnActivate)
            .boolean("blur", rule.blur)
            .boolean("blur_popups", rule.blurPopups)
            .boolean("blur_optimized", rule.blurOptimized)
            .real("opacity", 0.0, 1.0, rule.opacity)
            .real("blur_ignore_alpha", 0.0, 1.0, rule.blurIgnoreAlpha);
        if (const toml::node* n = keys.take("default_output")) {
          if (const auto value = n->value<std::string>()) {
            rule.defaultOutput = *value;
          } else {
            warnAt(n->source(), "ignoring window_rule.default_output (expected string)");
          }
        }

        if (const toml::node* n = keys.take("default_size")) {
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
            warnAt(n->source(), "ignoring window_rule.default_size (expected [width, height] positive integers)");
          } else {
            rule.defaultSize = parsed;
          }
        }

        if (const toml::node* n = keys.take("default_width")) {
          const auto value = n->value<double>();
          if (!value || std::isnan(*value)) {
            warnAt(n->source(), "ignoring window_rule.default_width (expected number 0.1-1.0)");
          } else {
            const double used = std::clamp(*value, 0.1, 1.0);
            if (used != *value) {
              warnAt(n->source(), "window_rule.default_width = {} out of range, clamped to {}", *value, used);
            }
            rule.defaultWidth = used;
          }
        }

        if (const toml::node* n = keys.take("default_workspace")) {
          const auto value = n->value<std::int64_t>();
          if (!value || *value < 1 || *value > static_cast<std::int64_t>(kMaxWorkspaces)) {
            warnAt(n->source(), "ignoring window_rule.default_workspace (expected integer 1-{})", kMaxWorkspaces);
          } else {
            rule.defaultWorkspace = static_cast<int>(*value);
          }
        }

        loaded.windowRules.push_back(std::move(rule));
      }
    }

    void readLayerRules(Section& root, Config& loaded) {
      const toml::node* node = root.take("layer_rule");
      if (node == nullptr) {
        return;
      }
      const auto* rules = node->as_array();
      if (rules == nullptr) {
        warnAt(node->source(), "ignoring layer_rule (expected [[layer_rule]] array of tables)");
        return;
      }

      for (const auto& entry : *rules) {
        const auto* section = entry.as_table();
        if (section == nullptr) {
          warnAt(entry.source(), "ignoring layer_rule entry (expected table)");
          continue;
        }
        Section keys(*section, "layer_rule", configStore().mutableDiagnostics());

        LayerRule rule;

        if (const toml::node* matchNode = keys.take("match")) {
          if (const auto* match = matchNode->as_table()) {
            Section matchKeys(*match, "layer_rule.match", configStore().mutableDiagnostics());
            if (const toml::node* namespaceNode = matchKeys.take("namespace")) {
              if (const auto value = namespaceNode->value<std::string>()) {
                rule.namespacePattern = *value;
                try {
                  rule.namespaceRegex = std::regex(rule.namespacePattern);
                } catch (const std::regex_error& error) {
                  warnAt(namespaceNode->source(), "invalid regex in layer_rule.match.namespace: {}", error.what());
                  continue;
                }
              } else {
                warnAt(namespaceNode->source(), "ignoring layer_rule.match.namespace (expected string)");
              }
            }
          } else {
            warnAt(matchNode->source(), "ignoring layer_rule.match (expected table)");
          }
        }

        keys.boolean("blur", rule.blur)
            .boolean("blur_popups", rule.blurPopups)
            .real("blur_ignore_alpha", 0.0, 1.0, rule.ignoreAlpha)
            .boolean("blur_optimized", rule.optimized);

        loaded.layerRules.push_back(std::move(rule));
      }
    }

    bool parseInto(Config& out) {
      ConfigStore& store = configStore();
      store.beginLoad();

      std::error_code error;
      if (!std::filesystem::is_regular_file(store.rootPath(), error) || error) {
        return false;
      }

      try {
        auto result = configmerge::mergeWithIncludes(store.rootPath());
        store.setMissingIncludes(result.missingIncludes);
        for (auto& diagnostic : result.diagnostics) {
          store.addDiagnostic(std::move(diagnostic));
        }
        for (const auto& path : result.loadedFiles) {
          store.addWatchPath(path);
        }
        if (result.hadParseError) {
          return false;
        }

        Config loaded;
        {
          Section root(result.merged, "", store.mutableDiagnostics());
          readColors(root, loaded);
          readAppearance(root, loaded);
          readOverview(root, loaded);
          readLayout(root, loaded);
          readGeneral(root, loaded);
          readEnvironment(root, loaded);
          readWorkspaceSettings(root, loaded);
          readInput(root, loaded);
          readOutputs(root, loaded);
          readKeybinds(root, loaded);
          readWindowRules(root, loaded);
          readLayerRules(root, loaded);
          readWorkspaces(root, loaded);
        }

        // Reject config if any error-level diagnostics were emitted.
        const bool hasErrors = std::ranges::any_of(configStore().diagnostics(), [](const ConfigDiagnostic& d) {
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
  const Config::Input::Device* Config::Input::findDevice(std::string_view name) const {
    const auto found = std::ranges::find_if(devices, [name](const Device& device) { return device.name == name; });
    return found == devices.end() ? nullptr : &*found;
  }

  ConfigStore& configStore() {
    static ConfigStore store;
    return store;
  }

  const Config& config() { return configStore().config(); }

  const std::vector<ConfigDiagnostic>& configDiagnostics() { return configStore().diagnostics(); }

  const std::filesystem::path& configRootPath() { return configStore().rootPath(); }

  bool configFileMissing() { return configStore().fileMissing(); }

  bool configHasMissingIncludes() { return configStore().missingIncludes(); }

  namespace {
    // Whether the root config actually exists on disk right now, which is not the
    // same as whether parsing succeeded: defaults are a valid way to run.
    bool rootFileMissing(const std::filesystem::path& root) {
      std::error_code ec;
      return !std::filesystem::is_regular_file(root, ec) || static_cast<bool>(ec);
    }
  } // namespace

  void ConfigStore::load(const char* explicitPath) {
    setRootPath(
        explicitPath == nullptr ? defaultConfigPath() : std::filesystem::path(explicitPath), explicitPath != nullptr
    );

    Config loaded;
    loaded.keybinds = defaultKeybinds();
    if (!parseInto(loaded) && rootFileMissing(m_rootPath)) {
      if (m_explicitPath) {
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr, std::format("config file not found: {}", m_rootPath.string())
        );
      } else {
        kLog.info("no config file found: {}, using defaults", m_rootPath.string());
      }
    }
    sortDiagnostics();
    (void)commit(std::move(loaded), rootFileMissing(m_rootPath));
  }

  ConfigReloadResult ConfigStore::reload() {
    Config loaded;
    const bool ok = parseInto(loaded);
    sortDiagnostics();
    if (!ok) {
      kLog.warn("config reload failed; keeping previous configuration");
      return {};
    }
    return commit(std::move(loaded), rootFileMissing(m_rootPath));
  }

  void loadConfig(const char* explicitPath) { configStore().load(explicitPath); }

  ConfigReloadResult reloadConfig() { return configStore().reload(); }

  const std::vector<std::filesystem::path>& configWatchPaths() { return configStore().watchPaths(); }

} // namespace umbriel
