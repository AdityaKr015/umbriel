#include "config/config.h"

#include "config/config_diag.h"
#include "config/config_merge.h"
#include "config/keybind_parse.h"
#include "config/read_context.h"
#include "config/registry.h"
#include "config/resolve.h"
#include "config/section.h"
#include "config/store.h"
#include "config/value_parse.h"
#include "core/log.h"
#include "output/identity.h"
#include "umbriel_build_config.h"

// clang-format off
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace umbriel {

  namespace {

    constexpr Logger kLog("config");

    constexpr std::array<std::string_view, 7> kReservedEnvironmentNames{
        "WAYLAND_DISPLAY",     "WAYLAND_SOCKET",      "DISPLAY",          "UMBRIEL_SOCKET",
        "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
    };

    // Well past any real layout: a value this large already means "no limit".
    constexpr double kMaxFollowsMouseScroll = 100.0;

    std::string lowercase(std::string_view text) {
      std::string lowered(text);
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return lowered;
    }

    const registry::Choices<VrrMode>& vrrModes() {
      static const registry::Choices<VrrMode> choices{
          {.name = "disabled", .value = VrrMode::Disabled},
          {.name = "always", .value = VrrMode::Always},
          {.name = "fullscreen", .value = VrrMode::Fullscreen},
      };
      return choices;
    }

    const registry::Choices<HdrMode>& hdrModes() {
      static const registry::Choices<HdrMode> choices{
          {.name = "off", .value = HdrMode::Off},
          {.name = "on", .value = HdrMode::On},
          {.name = "auto", .value = HdrMode::Auto},
          {.name = "fullscreen", .value = HdrMode::Fullscreen},
      };
      return choices;
    }

    const registry::Choices<ContentType>& contentTypes() {
      static const registry::Choices<ContentType> choices{
          {.name = "none", .value = ContentType::None},
          {.name = "photo", .value = ContentType::Photo},
          {.name = "video", .value = ContentType::Video},
          {.name = "game", .value = ContentType::Game},
      };
      return choices;
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

    std::optional<std::string> normalizePciAddress(std::string_view value) {
      constexpr std::array<size_t, 3> separators{4, 7, 10};
      if (value.size() != 12
          || value[separators[0]] != ':'
          || value[separators[1]] != ':'
          || value[separators[2]] != '.'
          || (value[8] != '0' && value[8] != '1')
          || value.back() < '0'
          || value.back() > '7') {
        return std::nullopt;
      }
      for (size_t index = 0; index < value.size(); ++index) {
        if (std::ranges::find(separators, index) != separators.end()) {
          continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
          return std::nullopt;
        }
      }
      return lowercase(value);
    }

    std::optional<std::string> readDrmPath(const toml::node& node, std::string_view context) {
      const auto value = node.value<std::string>();
      if (!value) {
        errorAt(node.source(), "{} must be a string", context);
        return std::nullopt;
      }
      if (value->empty()) {
        errorAt(node.source(), "{} cannot be empty", context);
        return std::nullopt;
      }
      if (value->contains('\0')) {
        errorAt(node.source(), "{} cannot contain NUL", context);
        return std::nullopt;
      }
      const std::filesystem::path path(*value);
      if (!path.is_absolute()) {
        errorAt(node.source(), R"({} must be an absolute path (got "{}"))", context, *value);
        return std::nullopt;
      }
      // Keep the exact spelling: lexical normalization changes the meaning of
      // `..` when an earlier path component is a symlink.
      return value;
    }

    std::optional<std::string> readDrmPciAddress(const toml::node& node, std::string_view context) {
      const auto value = node.value<std::string>();
      if (!value) {
        errorAt(node.source(), "{} entries must be strings", context);
        return std::nullopt;
      }
      const auto address = normalizePciAddress(*value);
      if (!address) {
        errorAt(
            node.source(), R"(invalid {} entry "{}" (expected domain:bus:slot.function, for example 0000:01:00.0))",
            context, *value
        );
      }
      return address;
    }

    template <typename Parse>
    void readDrmSelectorList(
        const toml::node& node, std::string_view context, std::vector<std::string>& target, Parse parse
    ) {
      const toml::array* values = node.as_array();
      if (values == nullptr) {
        errorAt(node.source(), "{} must be an array of strings", context);
        return;
      }
      for (const toml::node& entry : *values) {
        auto value = parse(entry, context);
        if (!value) {
          continue;
        }
        if (std::ranges::find(target, *value) != target.end()) {
          warnAt(entry.source(), R"(ignoring duplicate {} entry "{}")", context, *value);
          continue;
        }
        target.push_back(std::move(*value));
      }
    }

    std::filesystem::path userConfigPath() {
      if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
          xdgConfigHome != nullptr && xdgConfigHome[0] != '\0') {
        return std::filesystem::path(xdgConfigHome) / "umbriel/config.toml";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config/umbriel/config.toml";
      }
      return std::filesystem::path(".config/umbriel/config.toml");
    }

    enum class ConfigPathKind {
      Missing,
      RegularFile,
      Unavailable,
    };

    struct ConfigPathProbe {
      ConfigPathKind kind = ConfigPathKind::Missing;
      std::error_code error;
    };

    ConfigPathProbe probeConfigPath(const std::filesystem::path& path) {
      std::error_code error;
      const std::filesystem::file_status status = std::filesystem::status(path, error);
      if (error) {
        if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
          return {};
        }
        return {.kind = ConfigPathKind::Unavailable, .error = error};
      }
      if (status.type() == std::filesystem::file_type::not_found) {
        return {};
      }
      return {
          .kind = std::filesystem::is_regular_file(status) ? ConfigPathKind::RegularFile : ConfigPathKind::Unavailable,
          .error = {},
      };
    }

    std::vector<std::filesystem::path> defaultConfigCandidates() {
      std::vector<std::filesystem::path> candidates{userConfigPath()};
      const char* configuredDirs = std::getenv("XDG_CONFIG_DIRS");
      const std::string_view configDirs = configuredDirs != nullptr && configuredDirs[0] != '\0'
          ? std::string_view(configuredDirs)
          : std::string_view("/etc/xdg");
      size_t offset = 0;
      while (offset <= configDirs.size()) {
        const size_t separator = configDirs.find(':', offset);
        const std::string_view directory = configDirs.substr(offset, separator - offset);
        if (!directory.empty()) {
          candidates.push_back(std::filesystem::path(directory) / "umbriel/config.toml");
        }
        if (separator == std::string_view::npos) {
          break;
        }
        offset = separator + 1;
      }

      candidates.push_back(std::filesystem::path(kDataDir) / "umbriel/config.toml");
      return candidates;
    }

    struct ConfigSelection {
      std::filesystem::path root;
      std::vector<std::filesystem::path> watchPaths;
      bool found = false;
    };

    ConfigSelection selectDefaultConfig(const std::vector<std::filesystem::path>& candidates) {
      ConfigSelection selection{};
      selection.watchPaths = candidates;
      for (const std::filesystem::path& candidate : candidates) {
        if (probeConfigPath(candidate).kind != ConfigPathKind::Missing) {
          selection.root = candidate;
          selection.found = true;
          return selection;
        }
      }
      selection.root = candidates.empty() ? userConfigPath() : candidates.front();
      if (selection.watchPaths.empty()) {
        selection.watchPaths.push_back(selection.root);
      }
      return selection;
    }

    const registry::Choices<LayoutMode>& layoutModes() {
      static const registry::Choices<LayoutMode> choices{
          {.name = "scrolling", .value = LayoutMode::Scrolling},
          {.name = "dwindle", .value = LayoutMode::Dwindle},
          {.name = "master", .value = LayoutMode::Master},
      };
      return choices;
    }

    const registry::Choices<MasterPosition>& masterPositions() {
      static const registry::Choices<MasterPosition> choices{
          {.name = "left", .value = MasterPosition::Left},
          {.name = "right", .value = MasterPosition::Right},
          {.name = "center", .value = MasterPosition::Center},
      };
      return choices;
    }

    const registry::Choices<CenterFocusedColumn>& centerFocusedModes() {
      static const registry::Choices<CenterFocusedColumn> choices{
          {.name = "never", .value = CenterFocusedColumn::Never},
          {.name = "always", .value = CenterFocusedColumn::Always},
          {.name = "on_overflow", .value = CenterFocusedColumn::OnOverflow},
      };
      return choices;
    }

    const registry::Choices<FullscreenExitScope>& fullscreenExitScopes() {
      static const registry::Choices<FullscreenExitScope> choices{
          {.name = "tiled", .value = FullscreenExitScope::Tiled},
          {.name = "floating", .value = FullscreenExitScope::Floating},
          {.name = "pinned", .value = FullscreenExitScope::Pinned},
          {.name = "all", .value = FullscreenExitScope::All},
      };
      return choices;
    }

    // A string names one scope; an array combines several, and an empty array disables the behavior.
    std::optional<FullscreenExitScope> parseFullscreenExitScope(const toml::node& node, const std::string& path) {
      const std::string expected = registry::quotedList(registry::choiceNames(fullscreenExitScopes()));
      const auto find = [](std::string_view token) -> std::optional<FullscreenExitScope> {
        const auto& choices = fullscreenExitScopes();
        const auto found = std::ranges::find(choices, token, &registry::Choice<FullscreenExitScope>::name);
        return found == choices.end() ? std::nullopt : std::optional(found->value);
      };
      if (const auto* value = node.as_string()) {
        const std::optional<FullscreenExitScope> scope = find(value->get());
        if (!scope) {
          warnAt(node.source(), R"(ignoring {} "{}" (expected {}))", path, value->get(), expected);
        }
        return scope;
      }
      const auto* array = node.as_array();
      if (array == nullptr) {
        warnAt(node.source(), "ignoring {} (expected a string or an array of strings, each {})", path, expected);
        return std::nullopt;
      }
      auto combined = static_cast<uint8_t>(FullscreenExitScope::None);
      for (const toml::node& entry : *array) {
        const auto* value = entry.as_string();
        const std::optional<FullscreenExitScope> scope = value != nullptr ? find(value->get()) : std::nullopt;
        if (!scope) {
          warnAt(entry.source(), "ignoring {} entry (expected {})", path, expected);
          continue;
        }
        combined |= static_cast<uint8_t>(*scope);
      }
      return static_cast<FullscreenExitScope>(combined);
    }

    // The scope as it is written: `all` when it covers every scope, otherwise each scope it names.
    nlohmann::ordered_json fullscreenExitScopeJson(FullscreenExitScope scope) {
      nlohmann::ordered_json names = nlohmann::ordered_json::array();
      if (scope == FullscreenExitScope::All) {
        names.push_back("all");
        return names;
      }
      for (const auto& option : fullscreenExitScopes()) {
        if (option.value != FullscreenExitScope::All
            && (static_cast<uint8_t>(scope) & static_cast<uint8_t>(option.value)) != 0) {
          names.push_back(option.name);
        }
      }
      return names;
    }

    std::vector<std::string_view> splitWhitespace(std::string_view text) {
      std::vector<std::string_view> tokens;
      size_t offset = 0;
      while (offset < text.size()) {
        while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
          ++offset;
        }
        const size_t start = offset;
        while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) == 0) {
          ++offset;
        }
        if (start != offset) {
          tokens.push_back(text.substr(start, offset - start));
        }
      }
      return tokens;
    }

    std::optional<AccelProfile> parseAccelProfile(const toml::node& node, const std::string& path) {
      const auto* value = node.as_string();
      if (value == nullptr) {
        warnAt(node.source(), "{} must be a string", path);
        return std::nullopt;
      }
      const std::vector<std::string_view> tokens = splitWhitespace(value->get());
      if (tokens.empty()) {
        warnAt(node.source(), "{} cannot be empty", path);
        return std::nullopt;
      }
      const std::string profile = lowercase(tokens.front());
      if ((profile == "flat" || profile == "adaptive") && tokens.size() == 1) {
        return AccelProfile{
            .kind = profile == "flat" ? AccelProfile::Kind::Flat : AccelProfile::Kind::Adaptive,
            .step = 0.0,
            .points = {},
        };
      }
      if (profile != "custom" || tokens.size() < 4) {
        warnAt(
            node.source(), R"(invalid {} "{}" (expected "flat", "adaptive", or "custom <step> <points...>"))", path,
            value->get()
        );
        return std::nullopt;
      }

      std::vector<double> values;
      values.reserve(tokens.size() - 1);
      for (size_t index = 1; index < tokens.size(); ++index) {
        const std::string_view token = tokens[index];
        double number = 0.0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), number);
        if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(number)) {
          warnAt(node.source(), R"(invalid number "{}" in {})", token, path);
          return std::nullopt;
        }
        values.push_back(number);
      }
      if (values.front() <= 0.0) {
        warnAt(node.source(), "{} custom step must be greater than zero", path);
        return std::nullopt;
      }
      if (std::ranges::any_of(values.begin() + 1, values.end(), [](double point) { return point < 0.0; })) {
        warnAt(node.source(), "{} custom points must be non-negative", path);
        return std::nullopt;
      }
      return AccelProfile{
          .kind = AccelProfile::Kind::Custom,
          .step = values.front(),
          .points = std::vector<double>(values.begin() + 1, values.end()),
      };
    }

    // `accel_profile` on any device class whose struct has an `accelProfile` member.
    template <typename T> registry::Field<T> accelProfileField(std::optional<AccelProfile> T::* member) {
      return registry::custom<T>(
          "accel_profile",
          registry::KeyDescription("string").withValues({"flat", "adaptive"}).withFormat("accel_profile"),
          [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext&) {
            if (auto profile = parseAccelProfile(node, path)) {
              target.*member = std::move(profile);
            }
          }
      );
    }

    const registry::Fields<Config::Input::Touchpad::ScrollFactor>& scrollFactorAxes() {
      using Axes = Config::Input::Touchpad::ScrollFactor;
      static const registry::Fields<Axes> fields{
          registry::real("horizontal", 0.1, 10.0, &Axes::horizontal),
          registry::real("vertical", 0.1, 10.0, &Axes::vertical),
      };
      return fields;
    }

    std::optional<Config::Input::Touchpad::ScrollFactor>
    parseScrollFactor(const toml::node& node, const std::string& path, registry::ReadContext& context) {
      Config::Input::Touchpad::ScrollFactor factor;
      if (const toml::table* table = node.as_table()) {
        Section axes(*table, path, configStore().mutableDiagnostics());
        registry::readFields(axes, scrollFactorAxes(), factor, context);
        return factor;
      }
      const auto value = node.value<double>();
      if (!value || std::isnan(*value)) {
        warnAt(node.source(), "ignoring {} (expected number or table)", path);
        return std::nullopt;
      }
      const double used = std::clamp(*value, 0.1, 10.0);
      if (used != *value) {
        warnAt(node.source(), "{} = {} out of range, clamped to {}", path, *value, used);
      }
      return Config::Input::Touchpad::ScrollFactor{.horizontal = used, .vertical = used};
    }

    std::optional<std::array<float, 6>> parseCalibrationMatrix(const toml::node& node, const std::string& path) {
      const auto* array = node.as_array();
      if (array == nullptr || array->size() != 6) {
        warnAt(node.source(), "{} must be an array of 6 finite numbers", path);
        return std::nullopt;
      }
      std::array<float, 6> matrix{};
      for (size_t index = 0; index < 6; ++index) {
        const auto value = (*array)[index].value<double>();
        if (!value || !std::isfinite(*value)) {
          warnAt(node.source(), "{} must be an array of 6 finite numbers", path);
          return std::nullopt;
        }
        matrix[index] = static_cast<float>(*value);
      }
      return matrix;
    }

    std::optional<std::vector<double>> parseExtentPresets(const toml::node& node, const std::string& path) {
      const auto* array = node.as_array();
      if (array == nullptr || array->empty()) {
        warnAt(node.source(), "ignoring {} (expected non-empty array of numbers)", path);
        return std::nullopt;
      }

      std::vector<double> parsed;
      parsed.reserve(array->size());
      for (const auto& entry : *array) {
        const auto value = entry.value<double>();
        if (!value || std::isnan(*value)) {
          warnAt(node.source(), "ignoring {} (expected non-empty array of numbers)", path);
          return std::nullopt;
        }
        const double used = std::clamp(*value, 0.1, 1.0);
        if (used != *value) {
          warnAt(entry.source(), "{} = {} out of range, clamped to {}", path, *value, used);
        }
        parsed.push_back(used);
      }
      return parsed;
    }

    // The global `[layout]` and a workspace's `layout` override share their keys; `L` is Config::Layout or
    // WorkspaceLayoutOverrides, whose members are the same, optional in the override.
    template <typename L> const registry::Fields<L>& layoutFields() {
      using registry::boolean;
      using registry::choice;
      using registry::custom;
      using registry::Fields;
      using registry::integer;
      using registry::real;
      using registry::table;
      using Struts = decltype(L::struts);
      using Scrolling = decltype(L::scrolling);
      using Dwindle = decltype(L::dwindle);
      using Master = decltype(L::master);
      constexpr int kStrutLimit = 65535;

      static const Fields<Struts> struts{
          integer("left", -kStrutLimit, kStrutLimit, &Struts::left),
          integer("right", -kStrutLimit, kStrutLimit, &Struts::right),
          integer("top", -kStrutLimit, kStrutLimit, &Struts::top),
          integer("bottom", -kStrutLimit, kStrutLimit, &Struts::bottom),
      };
      static const Fields<Scrolling> scrolling{
          real("default_extent_fraction", 0.1, 1.0, &Scrolling::defaultExtentFraction),
          boolean("center_underfull_strip", &Scrolling::centerUnderfullStrip),
          choice("center_focused", &Scrolling::centerFocused, centerFocusedModes()),
      };
      static const Fields<Dwindle> dwindle{
          boolean("preserve_split", &Dwindle::preserveSplit),
      };
      static const Fields<Master> master{
          choice("position", &Master::position, masterPositions()),
          real("default_width_fraction", 0.1, 0.9, &Master::defaultWidthFraction),
          boolean("new_on_top", &Master::newOnTop),
          boolean("new_becomes_master", &Master::newBecomesMaster),
      };
      static const Fields<L> fields{
          choice("mode", &L::mode, layoutModes()),
          integer("gap", 0, 500, &L::gap),
          table("struts", &L::struts, struts),
          custom<L>(
              "extent_presets", registry::KeyDescription("float_array").withRange(0.1, 1.0),
              [](const toml::node& node, const std::string& path, L& target, registry::ReadContext&) {
                if (auto presets = parseExtentPresets(node, path)) {
                  target.extentPresets = std::move(*presets);
                }
              },
              [](const L& defaults) { return registry::detail::toJson(defaults.extentPresets); }
          ),
          custom<L>(
              "new_exits_fullscreen",
              registry::KeyDescription("enum_or_array").withValues(registry::choiceNames(fullscreenExitScopes())),
              [](const toml::node& node, const std::string& path, L& target, registry::ReadContext&) {
                registry::assign(target.newExitsFullscreen, parseFullscreenExitScope(node, path));
              },
              [](const L& defaults) -> nlohmann::ordered_json {
                if constexpr (std::is_same_v<L, Config::Layout>) {
                  return fullscreenExitScopeJson(defaults.newExitsFullscreen);
                } else {
                  return defaults.newExitsFullscreen ? fullscreenExitScopeJson(*defaults.newExitsFullscreen) : nullptr;
                }
              }
          ),
          table("scrolling", &L::scrolling, scrolling),
          table("dwindle", &L::dwindle, dwindle),
          table("master", &L::master, master),
      };
      return fields;
    }

    const registry::Fields<ScratchpadConfig>& scratchpadFields() {
      static const registry::Fields<ScratchpadConfig> fields{
          registry::custom<ScratchpadConfig>(
              "name", registry::KeyDescription("string"),
              [](const toml::node& node, const std::string& path, ScratchpadConfig& target,
                 registry::ReadContext& context) {
                const auto name = node.value<std::string>();
                if (!name) {
                  errorAt(node.source(), "{} must be a string", path);
                } else if (name->empty()) {
                  errorAt(node.source(), "{} must not be empty", path);
                } else if (*name == "default") {
                  errorAt(node.source(), "{} 'default' is reserved for the implicit scratchpad", path);
                } else {
                  target.name = *name;
                  return;
                }
                context.entryRejected = true;
              }
          ),
      };
      return fields;
    }

    bool acceptScratchpad(
        Section& keys, const toml::node& entry, ScratchpadConfig& scratchpad, Config& loaded,
        registry::ReadContext& context
    ) {
      if (context.entryRejected) {
        return false;
      }
      const toml::node* nameNode = keys.node("name");
      if (nameNode == nullptr) {
        errorAt(entry.source(), "{} must set name", keys.name());
        return false;
      }
      if (std::ranges::any_of(loaded.scratchpads, [&](const ScratchpadConfig& existing) {
            return existing.name == scratchpad.name;
          })) {
        errorAt(nameNode->source(), "{}.name duplicates scratchpad name '{}'", keys.name(), scratchpad.name);
        return false;
      }
      return true;
    }

    std::optional<std::string> scratchpadSelectorError(const Config& loaded, const Keybind& binding) {
      const auto* scratchpad = payloadIf<ScratchpadArg>(binding);
      if (scratchpad == nullptr) {
        return std::nullopt;
      }
      if (loaded.scratchpads.empty()) {
        if (!scratchpad->name.empty() && scratchpad->name != "default") {
          return std::format("unknown scratchpad '{}'", scratchpad->name);
        }
        return std::nullopt;
      }
      if (scratchpad->name.empty()) {
        return std::string{"scratchpad name required"};
      }
      const bool configured = std::ranges::any_of(loaded.scratchpads, [&](const ScratchpadConfig& candidate) {
        return candidate.name == scratchpad->name;
      });
      if (configured) {
        return std::nullopt;
      }
      return std::format("unknown scratchpad '{}'", scratchpad->name);
    }

    std::optional<std::string> scratchpadTargetError(const Config& loaded, std::string_view name) {
      if (loaded.scratchpads.empty()) {
        return name == "default" ? std::nullopt : std::optional{std::format("unknown scratchpad '{}'", name)};
      }
      const bool configured = std::ranges::any_of(loaded.scratchpads, [name](const ScratchpadConfig& candidate) {
        return candidate.name == name;
      });
      return configured ? std::nullopt : std::optional{std::format("unknown scratchpad '{}'", name)};
    }

    // A non-empty string a workspace rule selects by; anything else is an error.
    registry::Field<WorkspaceConfig> workspaceSelector(std::string_view key, std::string WorkspaceConfig::* member) {
      return registry::custom<WorkspaceConfig>(
          key, registry::KeyDescription("string"),
          [member](const toml::node& node, const std::string& path, WorkspaceConfig& target, registry::ReadContext&) {
            if (const auto value = node.value<std::string>()) {
              if (value->empty()) {
                errorAt(node.source(), "{} must not be empty", path);
              } else {
                target.*member = *value;
              }
            } else {
              errorAt(node.source(), "{} must be a string", path);
            }
          }
      );
    }

    const registry::Fields<WorkspaceConfig>& workspaceFields() {
      static const registry::Fields<WorkspaceConfig> fields{
          workspaceSelector("name", &WorkspaceConfig::name),
          workspaceSelector("output", &WorkspaceConfig::output),
          registry::custom<WorkspaceConfig>(
              "index", registry::KeyDescription("int").withRange(1, kMaxWorkspaces),
              [](const toml::node& node, const std::string& path, WorkspaceConfig& target, registry::ReadContext&) {
                const auto value = node.value<std::int64_t>();
                if (!value || *value < 1 || *value > static_cast<std::int64_t>(kMaxWorkspaces)) {
                  errorAt(node.source(), "{} must be an integer from 1 to {}", path, kMaxWorkspaces);
                } else {
                  target.index = static_cast<int>(*value);
                }
              }
          ),
          registry::table("layout", &WorkspaceConfig::layout, layoutFields<WorkspaceLayoutOverrides>()),
      };
      return fields;
    }

    void readWorkspaces(Section& root, Config& loaded, registry::ReadContext& read) {
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
        WorkspaceConfig ws;
        {
          Section keys(*section, context, configStore().mutableDiagnostics());
          registry::readFields(keys, workspaceFields(), ws, read);
        }
        const bool hasName = !ws.name.empty();
        const bool hasIndex = ws.index.has_value();
        if (hasName == hasIndex) {
          errorAt(entry.source(), "{} must set exactly one of name or index", context);
        }

        entries.push_back({std::move(ws), entry.source(), entryIndex});
        ++entryIndex;
      }

      const auto sameSelector = [](const WorkspaceConfig& left, const WorkspaceConfig& right) {
        if (!outputNamesEqual(left.output, right.output) || left.index.has_value() != right.index.has_value()) {
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

      const size_t sentinelCount = loaded.workspaces.emptyAbove ? 2 : 1;
      const size_t namedCapacity = kMaxWorkspaces - sentinelCount;
      const auto reportDynamicNameOverflow = [&](std::string_view output) {
        std::vector<std::string> names;
        for (const ParsedEntry& entry : entries) {
          const WorkspaceConfig& ws = entry.ws;
          if (ws.name.empty() || ws.index) {
            continue;
          }
          const bool applies =
              output.empty() ? ws.output.empty() : ws.output.empty() || outputNamesEqual(ws.output, output);
          if (!applies || std::ranges::find(names, ws.name) != names.end()) {
            continue;
          }
          names.push_back(ws.name);
          if (names.size() <= namedCapacity) {
            continue;
          }
          const std::string context = std::format("workspace[{}]", entry.arrayIndex);
          const std::string target =
              output.empty() ? "unscoped dynamic outputs" : std::format("dynamic output '{}'", output);
          const std::string_view reservation = sentinelCount == 1
              ? "one workspace slot is reserved for the empty sentinel"
              : "two workspace slots are reserved for empty sentinels";
          errorAt(
              entry.source, "{} exceeds the limit of {} named workspaces for {} because {}", context, namedCapacity,
              target, reservation
          );
          return true;
        }
        return false;
      };

      const bool globalOverflow = reportDynamicNameOverflow({});
      if (!globalOverflow) {
        std::vector<std::string> checkedOutputs;
        for (const ParsedEntry& entry : entries) {
          if (entry.ws.name.empty()
              || entry.ws.output.empty()
              || std::ranges::any_of(checkedOutputs, [&](const std::string& output) {
                   return outputNamesEqual(output, entry.ws.output);
                 })) {
            continue;
          }
          checkedOutputs.push_back(entry.ws.output);
          const auto configured = std::ranges::find_if(loaded.outputs, [&](const OutputRule& output) {
            return outputNamesEqual(output.name, entry.ws.output);
          });
          if (configured == loaded.outputs.end() || !configured->workspaces) {
            reportDynamicNameOverflow(entry.ws.output);
          }
        }
      }

      for (auto& entry : entries) {
        loaded.workspaceRules.push_back(std::move(entry.ws));
      }
    }

    const registry::Fields<Config::Colors>& colorFields() {
      using registry::color;
      using C = Config::Colors;
      static const registry::Fields<C::Border> border{
          color("focused", &C::Border::focused),
          color("unfocused", &C::Border::unfocused),
          color("outer", &C::Border::outer),
      };
      static const registry::Fields<C::Overview> overview{
          color("background_tint", &C::Overview::backgroundTint),
          color("workspace_background", &C::Overview::workspaceBackground),
          color("badge", &C::Overview::badge),
      };
      static const registry::Fields<C> fields{
          color("background", &C::background),
          color("text_primary", &C::textPrimary),
          color("text_muted", &C::textMuted),
          color("accent_primary", &C::accentPrimary),
          color("accent_secondary", &C::accentSecondary),
          color("warning", &C::warning),
          color("error", &C::error),
          color("insert_hint", &C::insertHint),
          color("backdrop", &C::backdrop),
          color("shadow", &C::shadow),
          registry::table("border", &C::border, border),
          registry::table("overview", &C::overview, overview),
      };
      return fields;
    }

    std::optional<BezierCurve> parseBezier(const toml::node& node) {
      const auto* values = node.as_array();
      if (values == nullptr || values->size() != 4) {
        return std::nullopt;
      }
      const auto x1 = (*values)[0].value<double>();
      const auto y1 = (*values)[1].value<double>();
      const auto x2 = (*values)[2].value<double>();
      const auto y2 = (*values)[3].value<double>();
      if (!x1
          || !y1
          || !x2
          || !y2
          || !std::isfinite(*x1)
          || !std::isfinite(*y1)
          || !std::isfinite(*x2)
          || !std::isfinite(*y2)
          || *x1 < 0.0
          || *x1 > 1.0
          || *x2 < 0.0
          || *x2 > 1.0) {
        return std::nullopt;
      }
      return BezierCurve{.x1 = *x1, .y1 = *y1, .x2 = *x2, .y2 = *y2};
    }

    std::optional<SpringConfig> parseSpring(const toml::node& node) {
      const auto* table = node.as_table();
      if (table == nullptr || table->size() != 2) {
        return std::nullopt;
      }
      const toml::node* dampingNode = table->get("damping");
      const toml::node* stiffnessNode = table->get("stiffness");
      if (dampingNode == nullptr || stiffnessNode == nullptr) {
        return std::nullopt;
      }
      const auto damping = dampingNode->value<double>();
      const auto stiffness = stiffnessNode->value<double>();
      if (!damping
          || !stiffness
          || !std::isfinite(*damping)
          || !std::isfinite(*stiffness)
          || *damping < 0.01
          || *damping > 5.0
          || *stiffness < 1.0
          || *stiffness > 10000.0) {
        return std::nullopt;
      }
      return SpringConfig{.damping = *damping, .stiffness = *stiffness};
    }

    std::optional<AnimationCurve> parseAnimationCurve(
        std::string_view str, const std::map<std::string, BezierCurve>& beziers = {},
        const std::map<std::string, SpringConfig>& springs = {}
    ) {
      std::string s = lowercase(str);

      for (const auto& [name, curve] : beziers) {
        if (lowercase(name) == s) {
          return AnimationCurve{.easing = Easing::CustomBezier, .bezier = curve};
        }
      }
      for (const auto& [name, spring] : springs) {
        if (lowercase(name) == s) {
          return AnimationCurve{.easing = Easing::Spring, .spring = spring};
        }
      }

      return CurveRegistry::parse(str);
    }

    std::optional<AnimationCurve> parseCurve(
        const toml::node& node, const std::string& path, const std::map<std::string, BezierCurve>& beziers,
        const std::map<std::string, SpringConfig>& springs
    ) {
      const auto* value = node.as_string();
      if (value == nullptr) {
        warnAt(node.source(), "{} must be a string", path);
        return std::nullopt;
      }
      if (auto curve = parseAnimationCurve(value->get(), beziers, springs)) {
        return curve;
      }
      warnAt(node.source(), R"(invalid curve "{}" in {})", value->get(), path);
      return std::nullopt;
    }

    // Reads a string selector under `key`, reporting the section's own "(expected string)" warning through
    // Section::text so every caller shares one wording. Returns the value and its source location, or nullopt when
    // the key was absent or not a string.
    std::optional<std::pair<std::string, toml::source_region>> takeEffectSelector(Section& keys, std::string_view key) {
      const toml::node* node = keys.node(key);
      if (node == nullptr) {
        return std::nullopt;
      }
      std::string value;
      keys.text(key, value); // claims the key and warns if it is not a string
      if (!node->is_string()) {
        return std::nullopt;
      }
      return std::make_pair(std::move(value), node->source());
    }

    void addEffectReference(
        std::vector<EffectReference>& references, std::string context,
        const std::pair<std::string, toml::source_region>& selector, EffectKind kind, bool allowOff,
        std::function<void()> clear
    ) {
      references.push_back({
          .context = std::move(context),
          .name = selector.first,
          .kind = kind,
          .allowOff = allowOff,
          .source = selector.second,
          .clear = std::move(clear),
      });
    }

    // An effect preset selector, checked against the presets once every table is read.
    template <typename T>
    registry::Field<T> effectField(std::string_view key, std::string T::* member, EffectKind kind) {
      return registry::custom<T>(
          key, registry::KeyDescription("string").withFormat("effect"),
          [member, kind](const toml::node& node, const std::string& path, T& target, registry::ReadContext& context) {
            const auto value = node.value<std::string>();
            if (!node.is_string() || !value) {
              warnAt(node.source(), "ignoring {} (expected string)", path);
              return;
            }
            std::string& selected = target.*member;
            selected = *value;
            addEffectReference(context.effectReferences, path, {*value, node.source()}, kind, false, [&selected] {
              selected.clear();
            });
          },
          [member](const T& defaults) { return nlohmann::ordered_json(defaults.*member); }
      );
    }

    // Presets are named by the user, and which keys one takes depends on its kind.
    void readEffectPresets(Section& presets, Effects& effects, registry::ReadContext& context) {
      for (const auto& [key, entry] : presets.table()) {
        const std::string name(key.str());
        const std::string path = "effects.preset." + name;
        const auto* table = entry.as_table();
        if (table == nullptr) {
          warnAt(entry.source(), "ignoring {} (expected table)", path);
          continue;
        }
        if (name == kEffectOff) {
          warnAt(key.source(), "ignoring {} ('off' is reserved)", path);
          continue;
        }
        Section keys(*table, path, configStore().mutableDiagnostics());
        const toml::node* kindNode = keys.take("kind");
        const std::optional<EffectKind> kind =
            kindNode != nullptr ? parseEffectKind(kindNode->value<std::string>().value_or("")) : std::nullopt;
        if (!kind) {
          warnAt(
              kindNode != nullptr ? kindNode->source() : key.source(),
              "ignoring {} (kind must be animation|border|window|screen|cursor)", path
          );
          keys.freeform();
          continue;
        }
        EffectPreset preset;
        preset.name = name;
        preset.kind = *kind;
        auto shader = readShaderSource(keys, "shader", configStore().mutableDiagnostics());
        for (auto& watched : shader.watchPaths) {
          configStore().addWatchPath(std::move(watched));
        }
        if (shader.source) {
          preset.shader = std::move(*shader.source);
        } else if (keys.node("shader") == nullptr) {
          warnAt(key.source(), "{} has no shader; the preset is inert", path);
        }
        keys.boolean("palette", preset.palette);
        // The preset moves into the vector; register the overlay reference by index after the push.
        std::optional<std::pair<std::string, toml::source_region>> overlay;
        switch (*kind) {
        case EffectKind::Border: {
          double speed = preset.speed;
          keys.integer("padding", 0, 1024, preset.padding)
              .real("speed", 0.0, 10.0, speed)
              .boolean("animated", preset.animated);
          preset.speed = static_cast<float>(speed);
          overlay = takeEffectSelector(keys, "overlay");
          if (overlay) {
            preset.overlay = overlay->first;
          }
          keys.sub("light", [&](Section& light) {
            BorderLight settings;
            double intensity = settings.intensity;
            double threshold = settings.threshold;
            light.integer("spread", 1, 256, settings.spread)
                .real("intensity", 0.0, 4.0, intensity)
                .real("threshold", 0.0, 1.0, threshold);
            settings.intensity = static_cast<float>(intensity);
            settings.threshold = static_cast<float>(threshold);
            preset.light = settings;
          });
          break;
        }
        case EffectKind::Cursor:
          keys.integer("radius", 0, 4096, preset.radius);
          break;
        case EffectKind::Animation:
        case EffectKind::Window:
        case EffectKind::Screen:
          break;
        }
        effects.presets.push_back(std::move(preset));
        if (overlay) {
          const size_t index = effects.presets.size() - 1;
          addEffectReference(
              context.effectReferences, path + ".overlay", *overlay, EffectKind::Window, false,
              [&effects, index] { effects.presets[index].overlay.clear(); }
          );
        }
      }
    }

    const registry::Fields<Effects>& effectsFields() {
      using registry::KeyDescription;
      static const registry::Fields<Effects> fields{
          registry::integer("max_fps", 0, 240, &Effects::maxFps),
          registry::boolean("in_capture", &Effects::inCapture),
          effectField("border", &Effects::border, EffectKind::Border),
          effectField("window", &Effects::window, EffectKind::Window),
          effectField("screen", &Effects::screen, EffectKind::Screen),
          effectField("cursor", &Effects::cursor, EffectKind::Cursor),
          registry::map<Effects>(
              "preset", KeyDescription("table"),
              [](Section& presets, Effects& effects, registry::ReadContext& context) {
                readEffectPresets(presets, effects, context);
              },
              [] {
                registry::Descriptions keys;
                const auto add = [&keys](std::string_view key, KeyDescription description) {
                  description.path = key;
                  keys.push_back(std::move(description));
                };
                add("kind", KeyDescription("enum").withValues({"animation", "border", "window", "screen", "cursor"}));
                add("shader", KeyDescription("string").withFormat("path"));
                add("palette", KeyDescription("bool"));
                add("padding", KeyDescription("int").withRange(0, 1024));
                add("speed", KeyDescription("float").withRange(0.0, 10.0));
                add("animated", KeyDescription("bool"));
                add("overlay", KeyDescription("string").withFormat("effect"));
                add("light", KeyDescription("table"));
                add("light.spread", KeyDescription("int").withRange(1, 256));
                add("light.intensity", KeyDescription("float").withRange(0.0, 4.0));
                add("light.threshold", KeyDescription("float").withRange(0.0, 1.0));
                add("radius", KeyDescription("int").withRange(0, 4096));
                return keys;
              }()
          ),
      };
      return fields;
    }

    // Every recorded reference is checked against the final preset table. `clear` mutates `loaded` through
    // references captured while parsing, so this takes it non-const to say so.
    void validateEffectReferences(Config& loaded, std::vector<EffectReference>& references) {
      for (EffectReference& reference : references) {
        if (const auto error =
                effectReferenceError(loaded.effects, reference.name, reference.kind, reference.allowOff)) {
          warnAt(reference.source, "ignoring {} ({})", reference.context, *error);
          reference.clear();
        }
      }
    }

    // Every timeline under [animation], each fed by the shared duration and curve.
    template <typename F> void forEachTimeline(Config::Animation& animation, F&& apply) {
      apply(animation.windowsIn.durationMs, animation.windowsIn.curve);
      apply(animation.windowsOut.durationMs, animation.windowsOut.curve);
      apply(animation.windowsMove.durationMs, animation.windowsMove.curve);
      apply(animation.workspaces.durationMs, animation.workspaces.curve);
      apply(animation.overview.durationMs, animation.overview.curve);
      apply(animation.scratchpad.durationMs, animation.scratchpad.curve);
      apply(animation.border.durationMs, animation.border.curve);
      apply(animation.dimUnfocused.durationMs, animation.dimUnfocused.curve);
      apply(animation.layers.durationMs, animation.layers.curve);
    }

    // A curve key: a built-in easing, or one of the [animation.beziers] and [animation.springs] read before it.
    template <typename T> registry::Field<T> curveField(std::string_view key, AnimationCurve T::* member) {
      return registry::custom<T>(
          key, registry::KeyDescription("string").withFormat("curve"),
          [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext& context) {
            const Config::Animation& animation = context.loaded.animation;
            registry::assign(target.*member, parseCurve(node, path, animation.beziers, animation.springs));
          }
      );
    }

    // One animation event: its effect, whether it runs, `extra` keys of its own, and its timeline. duration_ms and
    // curve resolve together, because a spring derives its own length: a duration configured beside one reaches
    // nothing and has to say so rather than look honoured.
    template <typename E> registry::Fields<E> eventFields(registry::Fields<E> extra) {
      registry::Fields<E> fields{
          effectField("effect", &E::effect, EffectKind::Animation),
          registry::boolean("enabled", &E::enabled),
      };
      std::ranges::move(extra, std::back_inserter(fields));
      fields.push_back(registry::integer("duration_ms", 1, 10000, &E::durationMs));
      fields.push_back(curveField("curve", &E::curve));
      fields.push_back(registry::step<E>([](Section& s, E& event, registry::ReadContext&) {
        if (registry::configuredInteger(s, "duration_ms") && event.curve.easing == Easing::Spring) {
          warnAt(
              s.node("duration_ms")->source(), "{} has no effect: its spring curve sets its own length",
              s.qualified("duration_ms")
          );
        }
      }));
      return fields;
    }

    template <typename E> registry::Field<E> styleField(std::initializer_list<std::string_view> styles) {
      registry::Choices<std::string> choices;
      for (const std::string_view style : styles) {
        choices.push_back({.name = style, .value = std::string(style)});
      }
      return registry::choice("style", &E::style, std::move(choices));
    }

    const registry::Fields<Config::Animation>& animationFields() {
      using registry::boolean;
      using registry::real;
      using registry::table;
      using A = Config::Animation;
      static const registry::Fields<A::WindowsIn> windowsIn = eventFields<A::WindowsIn>({
          real("scale", 0.1, 1.0, &A::WindowsIn::scale),
          styleField<A::WindowsIn>({"popin", "zoom", "slide", "fade", "none"}),
      });
      static const registry::Fields<A::WindowsOut> windowsOut = eventFields<A::WindowsOut>({
          real("scale", 0.1, 1.0, &A::WindowsOut::scale),
          styleField<A::WindowsOut>({"fade", "slide", "popin", "zoom"}),
      });
      static const registry::Fields<A::WindowsMove> windowsMove = eventFields<A::WindowsMove>({});
      static const registry::Fields<A::Workspaces> workspaces = eventFields<A::Workspaces>({});
      static const registry::Fields<A::Overview> overview = [] {
        auto fields = eventFields<A::Overview>({});
        fields.push_back(curveField("workspace_curve", &A::Overview::workspaceCurve));
        return fields;
      }();
      static const registry::Fields<A::Scratchpad> scratchpad = eventFields<A::Scratchpad>({
          real("dim", 0.0, 1.0, &A::Scratchpad::dim),
          boolean("blur", &A::Scratchpad::blur),
          real("scale", 0.0, 1.0, &A::Scratchpad::scale),
          boolean("maximize", &A::Scratchpad::maximize),
          boolean("fullscreen", &A::Scratchpad::fullscreen),
      });
      static const registry::Fields<A::Border> border = eventFields<A::Border>({});
      static const registry::Fields<A::DimUnfocused> dimUnfocused = eventFields<A::DimUnfocused>({
          real("dim", 0.0, 1.0, &A::DimUnfocused::dim),
      });
      static const registry::Fields<A::Layers> layers = eventFields<A::Layers>({});
      static const registry::Fields<A::WindowsDrag> windowsDrag{
          boolean("physics", &A::WindowsDrag::physics),
      };
      static const registry::Fields<A> fields{
          boolean("enabled", &A::enabled),
          registry::map<A>(
              "beziers", registry::KeyDescription("float_array").withFormat("bezier"),
              [](Section& s, A& animation, registry::ReadContext&) {
                for (const auto& [name, value] : s.table()) {
                  if (auto bezier = parseBezier(value)) {
                    animation.beziers[std::string(name.str())] = *bezier;
                  } else {
                    warnAt(value.source(), "invalid bezier curve '{}'", name.str());
                  }
                }
              }
          ),
          registry::map<A>(
              "springs", registry::KeyDescription("table"),
              [](Section& s, A& animation, registry::ReadContext&) {
                for (const auto& [name, value] : s.table()) {
                  if (auto spring = parseSpring(value)) {
                    animation.springs[std::string(name.str())] = *spring;
                  } else {
                    warnAt(value.source(), "invalid spring config '{}'", name.str());
                  }
                }
              },
              [] {
                registry::Descriptions keys{
                    registry::KeyDescription("float").withRange(0.01, 5.0),
                    registry::KeyDescription("float").withRange(1.0, 10000.0),
                };
                keys[0].path = "damping";
                keys[1].path = "stiffness";
                return keys;
              }()
          ),
          registry::integer("duration_ms", 1, 10000, &A::durationMs),
          registry::step<A>([](Section& s, A& animation, registry::ReadContext&) {
            if (registry::configuredInteger(s, "duration_ms")) {
              forEachTimeline(animation, [&](int& duration, AnimationCurve&) { duration = animation.durationMs; });
            }
          }),
          registry::custom<A>(
              "curve", registry::KeyDescription("string").withFormat("curve"),
              [](const toml::node& node, const std::string& path, A& animation, registry::ReadContext&) {
                if (auto curve = parseCurve(node, path, animation.beziers, animation.springs)) {
                  animation.curve = *curve;
                  forEachTimeline(animation, [&](int&, AnimationCurve& target) { target = *curve; });
                }
              }
          ),
          table("windows_in", &A::windowsIn, windowsIn),
          table("windows_out", &A::windowsOut, windowsOut),
          table("windows_move", &A::windowsMove, windowsMove),
          table("workspaces", &A::workspaces, workspaces),
          table("overview", &A::overview, overview),
          table("scratchpad", &A::scratchpad, scratchpad),
          table("border", &A::border, border),
          table("dim_unfocused", &A::dimUnfocused, dimUnfocused),
          table("layers", &A::layers, layers),
          table("windows_drag", &A::windowsDrag, windowsDrag),
          // The shared duration reaches nothing once every timeline it feeds derives its own length.
          registry::step<A>([](Section& s, A& animation, registry::ReadContext&) {
            if (!registry::configuredInteger(s, "duration_ms")
                || animation.overview.workspaceCurve.easing != Easing::Spring) {
              return;
            }
            bool allSprings = true;
            forEachTimeline(animation, [&](int&, AnimationCurve& curve) {
              allSprings = allSprings && curve.easing == Easing::Spring;
            });
            if (allSprings) {
              warnAt(
                  s.node("duration_ms")->source(),
                  "animation.duration_ms has no effect: every animation curve is a spring"
              );
            }
          }),
      };
      return fields;
    }

    const registry::Fields<Config::Appearance>& appearanceFields() {
      using registry::boolean;
      using registry::integer;
      using registry::real;
      using A = Config::Appearance;
      static const registry::Fields<A::Blur> blur{
          boolean("enabled", &A::Blur::enabled),          boolean("optimized", &A::Blur::optimized),
          integer("passes", 0, 8, &A::Blur::passes),      integer("radius", 0, 100, &A::Blur::radius),
          real("noise", 0.0, 1.0, &A::Blur::noise),       real("brightness", 0.0, 2.0, &A::Blur::brightness),
          real("contrast", 0.0, 2.0, &A::Blur::contrast), real("saturation", 0.0, 2.0, &A::Blur::saturation),
      };
      static const registry::Fields<A::Shadow> shadow{
          boolean("enabled", &A::Shadow::enabled),
          integer("softness", 0, 200, &A::Shadow::softness),
          integer("offset_x", -200, 200, &A::Shadow::offsetX),
          integer("offset_y", -200, 200, &A::Shadow::offsetY),
      };
      static const registry::Fields<A> fields{
          integer("border_width", 0, 100, &A::borderWidth),
          integer("outer_border_width", 0, 100, &A::outerBorderWidth),
          integer("corner_radius", 0, 100, &A::cornerRadius),
          real("drag_opacity", 0.0, 1.0, &A::dragOpacity),
          boolean("prefer_no_csd", &A::preferNoCsd),
          boolean("opaque_fullscreen", &A::opaqueFullscreen),
          registry::table("blur", &A::blur, blur),
          registry::table("shadow", &A::shadow, shadow),
      };
      return fields;
    }

    // Overview badge keys: printable ASCII, unique ignoring case.
    std::optional<std::string> parseShortcutKeys(const toml::node& node, const std::string& path) {
      const auto value = node.value<std::string>();
      if (!value) {
        warnAt(node.source(), "ignoring {} (expected string)", path);
        return std::nullopt;
      }
      if (value->size() < 2) {
        warnAt(node.source(), "ignoring {} (expected at least 2 characters)", path);
        return std::nullopt;
      }

      std::string normalized;
      normalized.reserve(value->size());
      for (const unsigned char character : *value) {
        if (character < 0x21 || character > 0x7E) {
          warnAt(node.source(), "ignoring {} (invalid character 0x{:02X})", path, static_cast<unsigned int>(character));
          return std::nullopt;
        }
        const char lowered =
            character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        if (normalized.contains(lowered)) {
          warnAt(
              node.source(), R"(ignoring {} (duplicate key "{}" ignoring ASCII case))", path,
              static_cast<char>(character)
          );
          return std::nullopt;
        }
        normalized.push_back(lowered);
      }
      return value;
    }

    const registry::Fields<Config::Overview>& overviewFields() {
      using registry::boolean;
      using registry::real;
      using O = Config::Overview;
      static const registry::Fields<O> fields{
          real("zoom", 0.1, 0.75, &O::zoom),
          real("scroll_factor_horizontal", 0.1, 10.0, &O::scrollFactorHorizontal),
          real("scroll_factor_vertical", 0.1, 10.0, &O::scrollFactorVertical),
          boolean("background_blur", &O::backgroundBlur),
          boolean("workspace_wallpaper", &O::workspaceWallpaper),
          boolean("shortcuts", &O::shortcuts),
          registry::custom<O>(
              "shortcut_keys", registry::KeyDescription("string"),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                if (auto keys = parseShortcutKeys(node, path)) {
                  target.shortcutKeys = std::move(*keys);
                }
              },
              [](const O& defaults) { return nlohmann::ordered_json(defaults.shortcutKeys); }
          ),
      };
      return fields;
    }

    // A compositor action, as a keybind names it. Scratchpads it names must already be declared.
    std::optional<Keybind> parseActionKey(const toml::node& node, const std::string& path, const Config& loaded) {
      const auto value = node.value<std::string>();
      if (!value) {
        warnAt(node.source(), "{} must be a string", path);
        return std::nullopt;
      }
      Keybind bind;
      if (!parseAction(*value, bind)) {
        warnAt(node.source(), R"(invalid {} "{}")", path, *value);
        return std::nullopt;
      }
      if (const auto invalid = scratchpadSelectorError(loaded, bind)) {
        warnAt(node.source(), "ignoring {} ({})", path, *invalid);
        return std::nullopt;
      }
      return bind;
    }

    const registry::Fields<Config::HotCorners>& hotCornerFields() {
      using Corners = Config::HotCorners;
      using Corner = Config::HotCorner;
      static const registry::Fields<Corner> corner{
          registry::boolean("enabled", &Corner::enabled),
          registry::integer("delay_ms", 0, 10000, &Corner::delayMs),
          registry::custom<Corner>(
              "action", registry::KeyDescription("string").withFormat("action"),
              [](const toml::node& node, const std::string& path, Corner& target, registry::ReadContext& context) {
                if (auto bind = parseActionKey(node, path, context.loaded)) {
                  target.action = std::move(*bind);
                }
              }
          ),
      };
      // Corners are ordered top-left, top-right, bottom-left, bottom-right.
      static const registry::Fields<Corners> fields{
          registry::table<Corners>(
              "top_left", [](auto& c) -> auto& { return c.corners[0]; }, corner
          ),
          registry::table<Corners>(
              "top_right", [](auto& c) -> auto& { return c.corners[1]; }, corner
          ),
          registry::table<Corners>(
              "bottom_left", [](auto& c) -> auto& { return c.corners[2]; }, corner
          ),
          registry::table<Corners>("bottom_right", [](auto& c) -> auto& { return c.corners[3]; }, corner),
      };
      return fields;
    }

    const registry::Fields<Config::Workspaces>& workspaceSettingFields() {
      using W = Config::Workspaces;
      static const registry::Fields<W> fields{
          registry::boolean("back_and_forth", &W::backAndForth),
          registry::boolean("empty_above", &W::emptyAbove),
      };
      return fields;
    }

    const registry::Fields<Config::ScreenCast>& screenCastFields() {
      using S = Config::ScreenCast;
      static const registry::Fields<S> fields{
          registry::boolean("disable_dynamic_confirmation", &S::disableDynamicConfirmation),
      };
      return fields;
    }

    const registry::Fields<Config::General>& generalFields() {
      using registry::boolean;
      using G = Config::General;
      static const registry::Fields<G> fields{
          registry::choice(
              "mod_key", &G::modKey,
              {
                  {.name = "Super", .value = ModifierKey::Super},
                  {.name = "Logo", .value = ModifierKey::Super, .alias = true},
                  {.name = "Win", .value = ModifierKey::Super, .alias = true},
                  {.name = "Alt", .value = ModifierKey::Alt},
                  {.name = "Ctrl", .value = ModifierKey::Control},
                  {.name = "Control", .value = ModifierKey::Control, .alias = true},
                  {.name = "Shift", .value = ModifierKey::Shift},
              },
              registry::Case::Fold
          ),
          boolean("xwayland", &G::xwayland),
          boolean("show_cheatsheet", &G::showCheatsheet),
          boolean("focus_on_activate", &G::focusOnActivate),
          boolean("honor_restored_maximize", &G::honorRestoredMaximize),
          registry::strings("autostart", &G::autostart),
      };
      return fields;
    }

    void readDrm(Section& root, Config& loaded) {
      const toml::node* node = root.take("drm");
      if (node == nullptr) {
        return;
      }
      const toml::table* table = node->as_table();
      if (table == nullptr) {
        errorAt(node->source(), "drm must be a table");
        return;
      }

      for (const auto& [key, value] : *table) {
        if (key == "ignored_devices") {
          readDrmSelectorList(value, "drm.ignored_devices", loaded.drm.ignoredDevices, readDrmPath);
        } else if (key == "ignored_pci_addresses") {
          readDrmSelectorList(value, "drm.ignored_pci_addresses", loaded.drm.ignoredPciAddresses, readDrmPciAddress);
        } else {
          errorAt(value.source(), "unknown key drm.{}", key.str());
        }
      }
    }

    void readEnvironmentVariables(Section& s, Config::Environment& environment) {
      std::vector<std::pair<std::string, std::string>> parsed;
      parsed.reserve(s.table().size());
      for (const auto& [key, value] : s.table()) {
        const auto entry = value.value<std::string>();
        if (!entry) {
          warnAt(value.source(), "ignoring environment.{} (expected string)", key.str());
          continue;
        }
        if (!isEnvironmentVariableName(key.str())) {
          warnAt(key.source(), R"(ignoring environment key "{}" (expected [A-Za-z_][A-Za-z0-9_]*))", key.str());
          continue;
        }
        if (std::ranges::find(kReservedEnvironmentNames, key.str()) != kReservedEnvironmentNames.end()) {
          warnAt(key.source(), "ignoring environment.{} (reserved by Umbriel)", key.str());
          continue;
        }
        if (entry->contains('\0')) {
          warnAt(value.source(), "ignoring environment.{} (value contains NUL)", key.str());
          continue;
        }
        parsed.emplace_back(std::string(key.str()), *entry);
      }
      environment.variables = std::move(parsed);
    }

    const registry::Fields<Config::Events>& eventFields() {
      using E = Config::Events;
      static const registry::Fields<E> fields{
          registry::text("lid_close", &E::lidClose),
          registry::text("lid_open", &E::lidOpen),
      };
      return fields;
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

    // A device rule is kept once it names a device no earlier rule names. Its XKB keys are checked merged over the
    // session keyboard they override, and dropped together when that fails.
    bool acceptDevice(
        Section& keys, const toml::node& entry, Config::Input::Device& device, Config::Input& input,
        registry::ReadContext&
    ) {
      const std::string& context = keys.name();
      if (entry.as_table()->get("name") == nullptr) {
        errorAt(entry.source(), "{} must set name", context);
        return false;
      }
      if (device.name.empty()) {
        return false;
      }
      if (input.findDevice(device.name) != nullptr) {
        errorAt(entry.source(), "{} duplicates device '{}'", context, device.name);
        return false;
      }
      if (device.layout || device.variant || device.options) {
        Config::Input::Keyboard keyboard = input.keyboard;
        keyboard.layout = device.layout.value_or(keyboard.layout);
        keyboard.variant = device.variant.value_or(keyboard.variant);
        keyboard.options = device.options.value_or(keyboard.options);
        if (!validateKeyboardInput(keyboard, entry.source(), context)) {
          device.layout.reset();
          device.variant.reset();
          device.options.reset();
        }
      }
      return true;
    }

    const registry::Fields<Config::Input>& inputFields() {
      using registry::boolean;
      using registry::Case;
      using registry::choice;
      using registry::Choice;
      using registry::custom;
      using registry::Fields;
      using registry::integer;
      using registry::real;
      using registry::table;
      using registry::text;
      using In = Config::Input;

      static const std::vector<Choice<ClickMethod>> clickMethods{
          {.name = "button_areas", .value = ClickMethod::ButtonAreas},
          {.name = "clickfinger", .value = ClickMethod::ClickFinger},
      };
      static const std::vector<Choice<TapButtonMap>> tapButtonMaps{
          {.name = "left_right_middle", .value = TapButtonMap::LeftRightMiddle},
          {.name = "left_middle_right", .value = TapButtonMap::LeftMiddleRight},
      };
      static const std::vector<Choice<uint32_t>> scrollButtons{
          {.name = "MouseLeft", .value = BTN_LEFT},     {.name = "MouseRight", .value = BTN_RIGHT},
          {.name = "MouseMiddle", .value = BTN_MIDDLE}, {.name = "MouseBack", .value = BTN_SIDE},
          {.name = "MouseForward", .value = BTN_EXTRA},
      };

      static const Fields<In::Keyboard> keyboard{
          text("layout", &In::Keyboard::layout),
          text("variant", &In::Keyboard::variant),
          text("options", &In::Keyboard::options),
          integer("repeat_rate", 0, 1000, &In::Keyboard::repeatRate),
          integer("repeat_delay", 0, 10000, &In::Keyboard::repeatDelay),
          boolean("numlock_toggle", &In::Keyboard::numlockToggle),
          choice(
              "track_layout", &In::Keyboard::trackLayout,
              {{.name = "global", .value = TrackLayout::Global}, {.name = "window", .value = TrackLayout::Window}}
          ),
      };
      static const Fields<In::Touchpad> touchpad{
          boolean("tap", &In::Touchpad::tap),
          boolean("natural_scroll", &In::Touchpad::naturalScroll),
          boolean("left_handed", &In::Touchpad::leftHanded),
          real("sensitivity", -1.0, 1.0, &In::Touchpad::sensitivity),
          boolean("disable_while_typing", &In::Touchpad::disableWhileTyping),
          boolean("disable_on_external_mouse", &In::Touchpad::disableOnExternalMouse),
          custom<In::Touchpad>(
              "scroll_factor", registry::KeyDescription("float_or_table").withRange(0.1, 10.0),
              [](const toml::node& node, const std::string& path, In::Touchpad& target,
                 registry::ReadContext& context) {
                if (auto factor = parseScrollFactor(node, path, context)) {
                  target.scrollFactor = factor;
                }
              },
              nullptr,
              [] {
                registry::Descriptions axes;
                registry::describeFields(scrollFactorAxes(), {}, "", axes);
                return axes;
              }()
          ),
          accelProfileField(&In::Touchpad::accelProfile),
          choice("click_method", &In::Touchpad::clickMethod, clickMethods, Case::Fold),
          choice("tap_button_map", &In::Touchpad::tapButtonMap, tapButtonMaps, Case::Fold),
      };
      static const Fields<In::Mouse> mouse{
          boolean("natural_scroll", &In::Mouse::naturalScroll),
          boolean("left_handed", &In::Mouse::leftHanded),
          real("sensitivity", -1.0, 1.0, &In::Mouse::sensitivity),
          integer("scroll_wheel_step", 1, 1000, &In::Mouse::scrollWheelStep),
          boolean("scroll_button_lock", &In::Mouse::scrollButtonLock),
          accelProfileField(&In::Mouse::accelProfile),
          choice("scroll_button", &In::Mouse::scrollButton, scrollButtons, Case::Fold),
      };
      static const Fields<In::Tablet> tablet{
          boolean("enabled", &In::Tablet::enabled),
          text("map_to_output", &In::Tablet::mapToOutput),
          boolean("map_to_focused_output", &In::Tablet::mapToFocusedOutput),
          boolean("map_to_focused_window", &In::Tablet::mapToFocusedWindow),
          boolean("left_handed", &In::Tablet::leftHanded),
          custom<In::Tablet>(
              "calibration_matrix", registry::KeyDescription("float_array"),
              [](const toml::node& node, const std::string& path, In::Tablet& target, registry::ReadContext&) {
                if (auto matrix = parseCalibrationMatrix(node, path)) {
                  target.calibrationMatrix = matrix;
                }
              }
          ),
      };
      static const Fields<In::Touch> touch{
          boolean("enabled", &In::Touch::enabled),
          text("map_to_output", &In::Touch::mapToOutput),
      };
      static const Fields<In::Cursor> cursor{
          text("theme", &In::Cursor::theme),
          integer("size", 1, 512, &In::Cursor::size),
          boolean("hardware_cursor", &In::Cursor::hardwareCursor),
          boolean("follows_focus", &In::Cursor::followsFocus),
          boolean("hide_when_typing", &In::Cursor::hideWhenTyping),
          integer("hide_timeout_ms", 0, 3600000, &In::Cursor::hideTimeoutMs),
      };
      // The limit is measured in viewport widths and the quantity it is compared against is unbounded: revealing a
      // column three screens away is 3.0. The upper bound here is a nonsense-catcher, not a ceiling. Below zero would
      // refuse focus even for a window already fully visible, which disables hover focus rather than limiting it.
      static const Fields<In::Focus> focus{
          boolean("follows_mouse", &In::Focus::followsMouse),
          real("follows_mouse_max_scroll", 0.0, kMaxFollowsMouseScroll, &In::Focus::followsMouseMaxScroll),
      };
      static const Fields<In::Device> device{
          custom<In::Device>(
              "name", registry::KeyDescription("string"),
              [](const toml::node& node, const std::string& path, In::Device& target, registry::ReadContext&) {
                if (const auto name = node.value<std::string>(); name && !name->empty()) {
                  target.name = *name;
                } else {
                  errorAt(node.source(), "{} must be a non-empty string", path);
                }
              }
          ),
          text("layout", &In::Device::layout),
          text("variant", &In::Device::variant),
          text("options", &In::Device::options),
          integer("repeat_rate", 0, 1000, &In::Device::repeatRate),
          integer("repeat_delay", 0, 10000, &In::Device::repeatDelay),
          boolean("tap", &In::Device::tap),
          boolean("natural_scroll", &In::Device::naturalScroll),
          boolean("left_handed", &In::Device::leftHanded),
          real("sensitivity", -1.0, 1.0, &In::Device::sensitivity),
          boolean("disable_while_typing", &In::Device::disableWhileTyping),
          boolean("scroll_button_lock", &In::Device::scrollButtonLock),
          accelProfileField(&In::Device::accelProfile),
          choice("click_method", &In::Device::clickMethod, clickMethods, Case::Fold),
          choice("tap_button_map", &In::Device::tapButtonMap, tapButtonMaps, Case::Fold),
          choice("scroll_button", &In::Device::scrollButton, scrollButtons, Case::Fold),
      };
      static const Fields<In> fields{
          boolean("middle_click_paste", &In::middleClickPaste),
          boolean("client_window_drag", &In::clientWindowDrag),
          choice(
              "window_drag_toggle", &In::windowDragToggle,
              {{.name = "none", .value = WindowDragToggle::None},
               {.name = "floating", .value = WindowDragToggle::Floating},
               {.name = "pinned", .value = WindowDragToggle::Pinned}}
          ),
          table(
              "keyboard", &In::keyboard, keyboard,
              [](const toml::node& node, In::Keyboard& target, registry::ReadContext&) {
                if (!validateKeyboardInput(target, node.source(), "input.keyboard")) {
                  target.layout.clear();
                  target.variant.clear();
                  target.options.clear();
                }
              }
          ),
          table("touchpad", &In::touchpad, touchpad),
          table("mouse", &In::mouse, mouse),
          table("tablet", &In::tablet, tablet),
          table("touch", &In::touch, touch),
          table("cursor", &In::cursor, cursor),
          table("focus", &In::focus, focus),
          registry::rules("device", &In::devices, device, registry::Shape::Error, acceptDevice),
      };
      return fields;
    }

    OutputRule* findOutputRuleMutable(Config& loaded, const std::string& name) {
      const auto it = std::ranges::find_if(loaded.outputs, [&](const OutputRule& rule) {
        return outputNamesEqual(rule.name, name);
      });
      return it != loaded.outputs.end() ? &*it : nullptr;
    }

    // An effect preset selector on a rule. It is recorded by the rule's accept step, once the rule's place in its
    // array is known.
    template <typename T>
    registry::Field<T> optionalEffectField(std::string_view key, std::optional<std::string> T::* member) {
      return registry::custom<T>(
          key, registry::KeyDescription("string").withFormat("effect"),
          [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext&) {
            if (!node.is_string()) {
              warnAt(node.source(), "ignoring {} (expected string)", path);
              return;
            }
            target.*member = node.value<std::string>();
          }
      );
    }

    void recordRuleEffect(
        registry::ReadContext& context, Section& keys, std::string_view key, EffectKind kind,
        std::function<void()> clear
    ) {
      const toml::node* node = keys.node(key);
      if (node == nullptr || !node->is_string()) {
        return;
      }
      addEffectReference(
          context.effectReferences, keys.qualified(key), {*node->value<std::string>(), node->source()}, kind, true,
          std::move(clear)
      );
    }

    // A count, a list of names, or "dynamic".
    void parseOutputWorkspaces(const toml::node& node, const std::string& path, OutputRule& rule) {
      if (const auto count = node.value<std::int64_t>()) {
        if (*count < 1 || *count > static_cast<std::int64_t>(kMaxWorkspaces)) {
          errorAt(node.source(), "{} must be an integer from 1 to {}", path, kMaxWorkspaces);
        } else {
          rule.workspaces = static_cast<size_t>(*count);
        }
        return;
      }
      if (const auto* names = node.as_array()) {
        bool valid = true;
        if (names->empty() || names->size() > kMaxWorkspaces) {
          errorAt(node.source(), "{} must contain 1 to {} names", path, kMaxWorkspaces);
          valid = false;
        }
        std::vector<std::string> parsed;
        parsed.reserve(names->size());
        for (const auto& item : *names) {
          const auto value = item.value<std::string>();
          if (!value || value->empty()) {
            errorAt(item.source(), "{} entries must be non-empty strings", path);
            valid = false;
            continue;
          }
          if (std::ranges::find(parsed, *value) != parsed.end()) {
            errorAt(item.source(), "{} contains duplicate name '{}'", path, *value);
            valid = false;
            continue;
          }
          parsed.push_back(*value);
        }
        if (valid) {
          rule.workspaces = std::move(parsed);
        }
        return;
      }
      if (node.value<std::string>() != "dynamic") {
        errorAt(node.source(), R"({} must be a count, a name array, or "dynamic")", path);
      }
    }

    const registry::Fields<OutputRule>& outputFields() {
      using registry::boolean;
      using registry::custom;
      using registry::KeyDescription;
      using O = OutputRule;
      static const registry::Fields<O::Layout::Scrolling> scrolling{
          registry::real("default_extent_fraction", 0.1, 1.0, &O::Layout::Scrolling::defaultExtentFraction),
      };
      static const registry::Fields<O::Layout> layout{
          registry::table("scrolling", &O::Layout::scrolling, scrolling),
      };
      static const registry::Fields<O> fields{
          boolean("enabled", &O::enabled),
          boolean("tearing", &O::allowTearing),
          boolean("direct_scanout", &O::directScanout),
          optionalEffectField("screen_effect", &O::screenEffect),
          registry::table("layout", &O::layout, layout),
          registry::integer("min_workspaces", 1, static_cast<int>(kMaxWorkspaces), &O::minWorkspaces),
          boolean("cyclic_workspaces", &O::cyclicWorkspaces),
          registry::choice(
              "workspace_axis", &O::workspaceAxis,
              {{.name = "vertical", .value = WorkspaceAxis::Vertical},
               {.name = "horizontal", .value = WorkspaceAxis::Horizontal}}
          ),
          custom<O>(
              "workspaces", KeyDescription("int_or_string_array").withValues({"dynamic"}).withRange(1, kMaxWorkspaces),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                parseOutputWorkspaces(node, path, target);
              },
              [](const O&) { return nlohmann::ordered_json("dynamic"); }
          ),
          custom<O>(
              "mode", KeyDescription("string").withFormat("output_mode"),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto value = node.value<std::string>();
                OutputMode mode;
                if (!value || !parseOutputMode(*value, mode)) {
                  warnAt(node.source(), R"(ignoring {} (expected "WIDTHxHEIGHT" or "WIDTHxHEIGHT@HZ"))", path);
                } else {
                  target.mode = mode;
                }
              }
          ),
          custom<O>(
              "position", KeyDescription("int_array").withRange(-100000, 100000),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto* position = node.as_array();
                bool valid = position != nullptr && position->size() == 2;
                std::array<int, 2> parsed{};
                for (size_t index = 0; valid && index < parsed.size(); ++index) {
                  const auto value = (*position)[index].value<std::int64_t>();
                  valid = value.has_value();
                  if (valid) {
                    parsed[index] = static_cast<int>(
                        std::clamp(*value, static_cast<std::int64_t>(-100000), static_cast<std::int64_t>(100000))
                    );
                  }
                }
                if (!valid) {
                  warnAt(node.source(), "ignoring {} (expected [x, y] integers)", path);
                } else {
                  target.position = parsed;
                }
              }
          ),
          registry::real("scale", 0.25, 4.0, &O::scale),
          registry::choice("vrr", &O::vrr, vrrModes()),
          registry::choice("hdr", &O::hdr, hdrModes()),
          registry::real("sdr_white", 80.0, 1000.0, &O::sdrWhite),
          custom<O>(
              "bit_depth", KeyDescription("int").withRange(8, 10),
              [](const toml::node& node, const std::string& path, O& target, registry::ReadContext&) {
                const auto value = node.value<std::int64_t>();
                if (value && (*value == 8 || *value == 10)) {
                  target.bitDepth = static_cast<int>(*value);
                } else {
                  warnAt(node.source(), "ignoring {} (expected 8 or 10)", path);
                }
              },
              [](const O& defaults) { return nlohmann::ordered_json(defaults.bitDepth); }
          ),
          registry::choice(
              "transform", &O::transform,
              {{.name = "normal", .value = 0},
               {.name = "90", .value = 1},
               {.name = "180", .value = 2},
               {.name = "270", .value = 3},
               {.name = "flipped", .value = 4},
               {.name = "flipped-90", .value = 5},
               {.name = "flipped-180", .value = 6},
               {.name = "flipped-270", .value = 7}}
          ),
      };
      return fields;
    }

    // A later section for the same output replaces an earlier one.
    void acceptOutput(
        const toml::key& key, Section& keys, const toml::node&, OutputRule& rule, Config& loaded,
        registry::ReadContext& context
    ) {
      const std::string name(key.str());
      rule.name = name;
      if (std::ranges::any_of(loaded.outputs, [&](const OutputRule& existing) {
            return outputNamesEqual(existing.name, name);
          })) {
        warnAt(key.source(), "duplicate output section '{}'", name);
        // Drop the discarded section's references so they cannot clear the surviving rule's value by name.
        std::erase_if(context.effectReferences, [&](const EffectReference& reference) {
          return std::ranges::any_of(loaded.outputs, [&](const OutputRule& existing) {
            return outputNamesEqual(existing.name, name)
                && reference.context == "output." + existing.name + ".screen_effect";
          });
        });
        std::erase_if(loaded.outputs, [&](const OutputRule& existing) {
          return outputNamesEqual(existing.name, name);
        });
      }
      if (const toml::node* minNode = keys.node("min_workspaces"); minNode != nullptr && rule.workspaces) {
        errorAt(minNode->source(), "{} requires dynamic workspaces", keys.qualified("min_workspaces"));
      }
      loaded.outputs.push_back(std::move(rule));
      recordRuleEffect(context, keys, "screen_effect", EffectKind::Screen, [&loaded, name] {
        if (OutputRule* output = findOutputRuleMutable(loaded, name)) {
          output->screenEffect.reset();
        }
      });
    }

    // The keys a keybind written as a table takes besides its chord.
    struct KeybindOptions {
      std::optional<std::string> action;
      bool repeat = true;
      bool allowWhenLocked = false;
      bool allowWhenInhibited = false;
      int cooldownMs = 0;
      std::string submap;
    };

    const registry::Fields<KeybindOptions>& keybindFields() {
      using K = KeybindOptions;
      static const registry::Fields<K> fields{
          // A missing or non-string action is reported against the whole bind.
          registry::custom<K>(
              "action", registry::KeyDescription("string").withFormat("action"),
              [](const toml::node& node, const std::string&, K& target, registry::ReadContext&) {
                target.action = node.value<std::string>();
              }
          ),
          registry::boolean("repeat", &K::repeat),
          registry::boolean("allow_when_locked", &K::allowWhenLocked),
          registry::boolean("allow_when_inhibited", &K::allowWhenInhibited),
          registry::integer("cooldown_ms", 0, 3600000, &K::cooldownMs),
          registry::text("submap", &K::submap),
      };
      return fields;
    }

    void readKeybinds(Section& section, Config& loaded, registry::ReadContext& context) {
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
      for (const auto& [key, entry] : section.table()) {
        const std::string chord(key.str());
        KeybindOptions options;
        bool hasSubmapAfter = false;

        if (const auto* tbl = entry.as_table()) {
          Section bind(*tbl, "keybinds." + chord, configStore().mutableDiagnostics());
          registry::readFields(bind, keybindFields(), options, context);
          const toml::node* submapNode = bind.node("submap");
          hasSubmapAfter = submapNode != nullptr && submapNode->is_string();
          if (!options.action) {
            warnAt(entry.source(), "ignoring keybind '{}' (table needs an 'action' string)", chord);
            continue;
          }
        } else {
          options.action = entry.value<std::string>();
          if (!options.action) {
            warnAt(entry.source(), "ignoring keybind '{}' (expected string or table)", chord);
            continue;
          }
        }

        if (hasSubmapAfter && !validSubmapName(options.submap)) {
          warnAt(
              entry.source(),
              "ignoring keybind '{}' (submap must be a non-empty name without ']' and may not be 'disable')", chord
          );
          continue;
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
        if (hasSubmapAfter) {
          binding.submapAfter = SubmapArg{.name = std::move(options.submap)};
        }
        binding.repeat = options.repeat && !binding.modifierOnly && !binding.submapAfter.has_value();
        binding.allowWhenLocked = options.allowWhenLocked;
        binding.allowWhenInhibited = options.allowWhenInhibited;
        binding.cooldownMs = options.cooldownMs;
        if (!parseAction(*options.action, binding)) {
          warnAt(key.source(), "ignoring keybind '{}' (unknown action '{}')", chord, *options.action);
          continue;
        }

        if (const auto invalid = scratchpadSelectorError(loaded, binding)) {
          warnAt(key.source(), "ignoring keybind '{}' ({})", chord, *invalid);
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

    // libinput swallows the scroll button while it turns motion into scrolling, but a press released without any
    // motion still reaches the compositor as a click, so a bind on that button fires only in that case.
    void warnScrollButtonBinds(const Config& loaded) {
      const auto report = [&loaded](std::optional<uint32_t> button, std::string_view context) {
        if (!button) {
          return;
        }
        if (std::ranges::none_of(loaded.keybinds, [&](const Keybind& bind) { return bind.mouseButton == *button; })) {
          return;
        }
        const char* name = mouseButtonName(*button);
        warnNoSrc(
            "{} claims {} for scrolling, so binds on it fire only when it is released without motion", context,
            name != nullptr ? name : "it"
        );
      };
      report(loaded.input.mouse.scrollButton, "input.mouse.scroll_button");
      for (const Config::Input::Device& device : loaded.input.devices) {
        report(device.scrollButton, "input.device.scroll_button");
      }
    }

    // A regex pattern a rule matches with. One that does not compile rejects the rule.
    template <typename T>
    registry::Field<T> regexField(std::string_view key, std::string T::* pattern, std::regex T::* regex) {
      return registry::custom<T>(
          key, registry::KeyDescription("string").withFormat("regex"),
          [pattern, regex](const toml::node& node, const std::string& path, T& target, registry::ReadContext& context) {
            const auto value = node.value<std::string>();
            if (!value) {
              warnAt(node.source(), "ignoring {} (expected string)", path);
              return;
            }
            target.*pattern = *value;
            try {
              target.*regex = std::regex(*value);
            } catch (const std::regex_error& error) {
              warnAt(node.source(), "invalid regex in {}: {}", path, error.what());
              context.entryRejected = true;
            }
          }
      );
    }

    // A match table that is not a table leaves its rule selecting nothing it meant to, so the rule is dropped.
    template <typename T> void rejectUnlessTable(const toml::node& node, T&, registry::ReadContext& context) {
      if (!node.is_table()) {
        context.entryRejected = true;
      }
    }

    // `{ x, y, anchor }`, placed only when both coordinates are set and the anchor is valid.
    void parseDefaultPosition(const toml::node& node, const std::string& path, WindowRule& rule) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        warnAt(node.source(), "ignoring {} (expected table)", path);
        return;
      }
      static const registry::Choices<WindowPositionAnchor> anchors{
          {.name = "top_left", .value = WindowPositionAnchor::TopLeft},
          {.name = "top_right", .value = WindowPositionAnchor::TopRight},
          {.name = "bottom_left", .value = WindowPositionAnchor::BottomLeft},
          {.name = "bottom_right", .value = WindowPositionAnchor::BottomRight},
          {.name = "top", .value = WindowPositionAnchor::Top},
          {.name = "bottom", .value = WindowPositionAnchor::Bottom},
          {.name = "left", .value = WindowPositionAnchor::Left},
          {.name = "right", .value = WindowPositionAnchor::Right},
          {.name = "center", .value = WindowPositionAnchor::Center},
      };
      Section position(*table, path, configStore().mutableDiagnostics());
      std::optional<int> x;
      std::optional<int> y;
      position.integer("x", -100000, 100000, x).integer("y", -100000, 100000, y);
      std::optional<WindowPositionAnchor> anchor = WindowPositionAnchor::Center;
      if (const toml::node* anchorNode = position.take("anchor")) {
        anchor =
            registry::readChoice(position, *anchorNode, position.qualified("anchor"), anchors, registry::Case::Fold);
      }
      if (!x || !y) {
        warnAt(node.source(), "ignoring {} (x and y are required integers)", path);
      } else if (anchor) {
        rule.defaultPosition = WindowPosition{.x = *x, .y = *y, .anchor = *anchor};
      }
    }

    // A string that must not be empty.
    template <typename T>
    registry::Field<T> nonEmptyText(std::string_view key, std::optional<std::string> T::* member) {
      return registry::custom<T>(
          key, registry::KeyDescription("string"),
          [member](const toml::node& node, const std::string& path, T& target, registry::ReadContext&) {
            const auto value = node.value<std::string>();
            if (!value || value->empty()) {
              warnAt(node.source(), "ignoring {} (expected non-empty string)", path);
            } else {
              target.*member = *value;
            }
          }
      );
    }

    const registry::Fields<WindowRule>& windowRuleFields() {
      using registry::boolean;
      using registry::color;
      using registry::custom;
      using registry::integer;
      using registry::KeyDescription;
      using registry::real;
      using registry::strict;
      using W = WindowRule;
      const auto self = [](auto& rule) -> auto& { return rule; };
      // Any mistake in the match rejects the rule: one that selects less than it says would restyle other windows.
      static const registry::Fields<W> match{
          strict(regexField("app_id", &W::appIdPattern, &W::appIdRegex)),
          strict(regexField("title", &W::titlePattern, &W::titleRegex)),
          strict(regexField("xdg_tag", &W::xdgTagPattern, &W::xdgTagRegex)),
          strict(registry::choice("content_type", &W::matchContentType, contentTypes())),
          strict(boolean("is_focused", &W::matchFocused)),
          strict(boolean("is_floating", &W::matchFloating)),
          strict(boolean("is_pinned", &W::matchPinned)),
          strict(boolean("is_scratchpad", &W::matchScratchpad)),
          strict(boolean("is_alone", &W::matchAlone)),
          strict(boolean("at_startup", &W::matchAtStartup)),
      };
      static const registry::Fields<W> floatingSize{
          real("width", 0.1, 1.0, &W::defaultFloatingWidth),
          real("height", 0.1, 1.0, &W::defaultFloatingHeight),
      };
      static const registry::Fields<W> floatingSizePx{
          integer("width", 1, 100000, &W::defaultFloatingWidthPx),
          integer("height", 1, 100000, &W::defaultFloatingHeightPx),
      };
      static const registry::Fields<W> fields{
          registry::table<W>("match", self, match, rejectUnlessTable<W>),
          boolean("default_floating", &W::defaultFloating),
          boolean("default_fullscreen", &W::defaultFullscreen),
          boolean("default_maximize_to_edges", &W::defaultMaximizeToEdges),
          boolean("default_maximize", &W::defaultMaximize),
          boolean("default_focused", &W::defaultFocused),
          boolean("default_pinned", &W::defaultPinned),
          boolean("focus_on_activate", &W::focusOnActivate),
          boolean("tearing", &W::allowTearing),
          boolean("blur", &W::blur),
          boolean("blur_popups", &W::blurPopups),
          boolean("blur_optimized", &W::blurOptimized),
          real("opacity", 0.0, 1.0, &W::opacity),
          real("blur_ignore_alpha", 0.0, 1.0, &W::blurIgnoreAlpha),
          color("border_color_focused", &W::borderColorFocused),
          color("border_color_unfocused", &W::borderColorUnfocused),
          color("border_color_outer", &W::borderColorOuter),
          integer("border_width", 0, 100, &W::borderWidth),
          integer("outer_border_width", 0, 100, &W::outerBorderWidth),
          integer("corner_radius", 0, 100, &W::cornerRadius),
          boolean("shadow", &W::shadow),
          optionalEffectField("border_effect", &W::borderEffect),
          optionalEffectField("window_effect", &W::windowEffect),
          registry::table<W>("default_floating_size", self, floatingSize),
          registry::table<W>("default_floating_size_px", self, floatingSizePx),
          registry::choice("vrr", &W::vrr, vrrModes()),
          registry::choice("hdr", &W::hdr, hdrModes()),
          registry::text("default_output", &W::defaultOutput),
          custom<W>(
              "default_position", KeyDescription("table"),
              [](const toml::node& node, const std::string& path, W& target, registry::ReadContext&) {
                parseDefaultPosition(node, path, target);
              },
              nullptr,
              [] {
                registry::Descriptions keys{
                    KeyDescription("int").withRange(-100000, 100000),
                    KeyDescription("int").withRange(-100000, 100000),
                    KeyDescription("enum").withValues(
                        {"top_left", "top_right", "bottom_left", "bottom_right", "top", "bottom", "left", "right",
                         "center"}
                    ),
                };
                keys[0].path = "x";
                keys[1].path = "y";
                keys[2].path = "anchor";
                keys[2].defaultValue = "center";
                return keys;
              }()
          ),
          integer("default_scrolling_extent_px", 1, 100000, &W::defaultScrollingExtentPx),
          real("default_scrolling_extent", 0.1, 1.0, &W::defaultScrollingExtent),
          custom<W>(
              "default_workspace", KeyDescription("int_or_string").withRange(1, kMaxWorkspaces),
              [](const toml::node& node, const std::string& path, W& target, registry::ReadContext&) {
                if (const auto value = node.value<std::int64_t>()) {
                  if (*value >= 1 && *value <= static_cast<std::int64_t>(kMaxWorkspaces)) {
                    target.defaultWorkspace = WorkspaceReference{WorkspaceIndex{static_cast<size_t>(*value)}};
                    return;
                  }
                } else if (const auto name = node.value<std::string>(); name && !name->empty()) {
                  target.defaultWorkspace = WorkspaceReference{WorkspaceName{*name}};
                  return;
                }
                warnAt(node.source(), "ignoring {} (expected integer 1-{} or non-empty string)", path, kMaxWorkspaces);
              }
          ),
          custom<W>(
              "default_scratchpad", KeyDescription("string"),
              [](const toml::node& node, const std::string& path, W& target, registry::ReadContext& context) {
                const auto value = node.value<std::string>();
                if (!value || value->empty()) {
                  warnAt(node.source(), "ignoring {} (expected non-empty string)", path);
                } else if (const auto invalid = scratchpadTargetError(context.loaded, *value)) {
                  warnAt(node.source(), "ignoring {} ({})", path, *invalid);
                } else {
                  target.defaultScratchpad = *value;
                }
              }
          ),
          nonEmptyText("default_scrolling_column", &W::defaultScrollingColumn),
          integer(
              "default_scrolling_column_order", std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
              &W::defaultScrollingColumnOrder
          ),
      };
      return fields;
    }

    // The rule's effects are recorded once its place in the array is known.
    bool
    acceptWindowRule(Section& keys, const toml::node&, WindowRule&, Config& loaded, registry::ReadContext& context) {
      if (context.entryRejected) {
        return false;
      }
      const size_t index = loaded.windowRules.size();
      recordRuleEffect(context, keys, "border_effect", EffectKind::Border, [&loaded, index] {
        loaded.windowRules[index].borderEffect.reset();
      });
      recordRuleEffect(context, keys, "window_effect", EffectKind::Window, [&loaded, index] {
        loaded.windowRules[index].windowEffect.reset();
      });
      return true;
    }

    const registry::Fields<LayerRule>& layerRuleFields() {
      using L = LayerRule;
      static const registry::Fields<L> match{
          regexField("namespace", &L::namespacePattern, &L::namespaceRegex),
      };
      static const registry::Fields<L> fields{
          registry::table<L>(
              "match", [](auto& rule) -> auto& { return rule; }, match
          ),
          registry::boolean("blur", &L::blur),
          registry::boolean("blur_popups", &L::blurPopups),
          registry::real("blur_ignore_alpha", 0.0, 1.0, &L::ignoreAlpha),
          registry::boolean("blur_optimized", &L::optimized),
      };
      return fields;
    }

    // A security-context selector: a non-empty pattern that compiles, or the rule is dropped.
    registry::Field<SecurityContextRule> securityPattern(
        std::string_view key, std::string SecurityContextRule::* pattern, std::regex SecurityContextRule::* regex
    ) {
      return registry::custom<SecurityContextRule>(
          key, registry::KeyDescription("string").withFormat("regex"),
          [pattern, regex](
              const toml::node& node, const std::string& path, SecurityContextRule& target,
              registry::ReadContext& context
          ) {
            const auto value = node.value<std::string>();
            if (!value || value->empty()) {
              warnAt(node.source(), "ignoring {} (expected non-empty string)", path);
              context.entryRejected = true;
              return;
            }
            target.*pattern = *value;
            try {
              target.*regex = std::regex(*value);
            } catch (const std::regex_error& error) {
              warnAt(node.source(), "invalid regex in {}: {}", path, error.what());
              context.entryRejected = true;
            }
          }
      );
    }

    // Any mistake rejects the whole entry: a rule missing its selector would apply to every restricted client.
    const registry::Fields<SecurityContextRule>& securityContextRuleFields() {
      using S = SecurityContextRule;
      static const registry::Fields<S> match{
          securityPattern("sandbox_engine", &S::sandboxEnginePattern, &S::sandboxEngineRegex),
          securityPattern("app_id", &S::appIdPattern, &S::appIdRegex),
      };
      static const registry::Fields<S> fields{
          registry::table<S>(
              "match", [](auto& rule) -> auto& { return rule; }, match,
              [](const toml::node& node, S&, registry::ReadContext& context) {
                const toml::table* table = node.as_table();
                if (table == nullptr) {
                  context.entryRejected = true;
                } else if (!registry::declaresAll(match, *table)) {
                  warnAt(node.source(), "ignoring security_context_rule (unknown key in match)");
                  context.entryRejected = true;
                }
              }
          ),
          registry::strings("allow_globals", &S::allowGlobals),
      };
      return fields;
    }

    bool acceptSecurityContextRule(
        Section& keys, const toml::node& entry, SecurityContextRule& rule, Config&, registry::ReadContext&
    ) {
      // The filter refuses this global regardless; warning here tells the user why.
      if (std::erase(rule.allowGlobals, "wp_security_context_manager_v1") > 0) {
        warnAt(
            entry.source(),
            "ignoring wp_security_context_manager_v1 in security_context_rule.allow_globals (nested contexts stay "
            "blocked)"
        );
      }
      bool valid = true;
      if (rule.allowGlobals.empty()) {
        warnAt(entry.source(), "ignoring security_context_rule (allow_globals is empty)");
        valid = false;
      }
      if (!keys.allKeysKnown()) {
        warnAt(entry.source(), "ignoring security_context_rule (unknown key)");
        valid = false;
      }
      return valid;
    }

    // Every top-level table, in reading order: a table may depend on one read before it, as hot corner actions
    // depend on the scratchpads they name.
    const registry::Fields<Config>& configFields() {
      using registry::KeyDescription;
      using registry::Shape;
      using registry::table;
      static const registry::Fields<Config> fields{
          table("colors", &Config::colors, colorFields()),
          table("effects", &Config::effects, effectsFields()),
          table("animation", &Config::animation, animationFields()),
          table("appearance", &Config::appearance, appearanceFields()),
          table("overview", &Config::overview, overviewFields()),
          registry::rules("scratchpad", &Config::scratchpads, scratchpadFields(), Shape::Error, acceptScratchpad),
          table("hot_corners", &Config::hotCorners, hotCornerFields()),
          table("layout", &Config::layout, layoutFields<Config::Layout>()),
          table("general", &Config::general, generalFields()),
          // Unknown keys here are errors, not warnings: a misspelled exclusion must not silently claim a GPU.
          registry::handRead<Config>(
              "drm", KeyDescription("table"),
              [] {
                registry::Descriptions keys{
                    KeyDescription("string_array").withFormat("path"),
                    KeyDescription("string_array").withFormat("pci_address"),
                };
                keys[0].path = ".ignored_devices";
                keys[1].path = ".ignored_pci_addresses";
                return keys;
              }(),
              [](Section& s, Config& c, registry::ReadContext&) { readDrm(s, c); }
          ),
          registry::map<Config>(
              "environment", KeyDescription("string"),
              [](Section& s, Config& c, registry::ReadContext&) { readEnvironmentVariables(s, c.environment); }, {},
              "table"
          ),
          table("events", &Config::events, eventFields()),
          table("workspaces", &Config::workspaces, workspaceSettingFields()),
          table("screencast", &Config::screenCast, screenCastFields()),
          table("input", &Config::input, inputFields()),
          registry::namedTables<Config>("output", outputFields(), acceptOutput),
          registry::map<Config>(
              "keybinds", KeyDescription("string_or_table").withFormat("action"), readKeybinds,
              [] {
                registry::Descriptions keys;
                registry::describeFields(keybindFields(), KeybindOptions{}, "", keys);
                return keys;
              }()
          ),
          registry::rules("window_rule", &Config::windowRules, windowRuleFields(), Shape::Warning, acceptWindowRule),
          registry::rules("layer_rule", &Config::layerRules, layerRuleFields(), Shape::Warning),
          registry::rules(
              "security_context_rule", &Config::securityContextRules, securityContextRuleFields(), Shape::Warning,
              acceptSecurityContextRule
          ),
          registry::handRead<Config>(
              "workspace", KeyDescription("array_of_tables"),
              [] {
                registry::Descriptions keys;
                registry::describeFields(workspaceFields(), WorkspaceConfig{}, "[]", keys);
                return keys;
              }(),
              readWorkspaces
          ),
          // Read before any table, by the include merge; declared here so the schema lists it.
          registry::handRead<Config>(
              "include", KeyDescription("table"),
              [] {
                registry::Descriptions keys{
                    KeyDescription("string_array").withFormat("path"),
                    KeyDescription("table"),
                    KeyDescription("string_array").withFormat("path"),
                };
                keys[0].path = ".files";
                keys[1].path = ".optional";
                keys[2].path = ".optional.files";
                return keys;
              }(),
              [](Section&, Config&, registry::ReadContext&) {}
          ),
      };
      return fields;
    }

    enum class ConfigParseOutcome : uint8_t {
      Loaded,
      Missing,
      DefaultsAllowed,
      Fatal,
    };

    bool hasRequestedDrmPolicy(const toml::table& root) {
      const toml::node* node = root.get("drm");
      if (node == nullptr) {
        return false;
      }
      const toml::table* table = node->as_table();
      return table == nullptr || !table->empty();
    }

    ConfigParseOutcome parseInto(
        Config& out, const std::filesystem::path& rootPath, const std::vector<std::filesystem::path>& watchPaths
    ) {
      ConfigStore& store = configStore();
      store.beginLoad(watchPaths);

      const ConfigPathProbe root = probeConfigPath(rootPath);
      if (root.kind == ConfigPathKind::Missing) {
        return ConfigParseOutcome::Missing;
      }
      if (root.kind == ConfigPathKind::Unavailable) {
        const std::string reason = root.error ? root.error.message() : "not a regular file";
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr,
            std::format("cannot inspect config file {}: {}", rootPath.string(), reason)
        );
        return ConfigParseOutcome::Fatal;
      }

      bool drmPolicyRequested = false;
      try {
        auto result = configmerge::mergeWithIncludes(rootPath);
        drmPolicyRequested = hasRequestedDrmPolicy(result.merged);
        store.setMissingIncludes(result.missingIncludes);
        for (auto& diagnostic : result.diagnostics) {
          store.addDiagnostic(std::move(diagnostic));
        }
        for (const auto& path : result.loadedFiles) {
          store.addWatchPath(path);
        }
        if ((result.missingIncludes || result.missingOptionalIncludes) && result.merged.contains("drm")) {
          emitDiag(
              ConfigDiagnostic::Severity::Error, nullptr, "cannot safely load DRM policy while an include is missing"
          );
          return ConfigParseOutcome::Fatal;
        }
        if (result.hadError) {
          // Invalid syntax or include directives can hide DRM policy intent.
          // Do not silently start with defaults or a partial exclusion list.
          return ConfigParseOutcome::Fatal;
        }

        Config loaded;
        std::vector<EffectReference> effectReferences;
        {
          Section root(result.merged, "", store.mutableDiagnostics());
          registry::ReadContext context{.loaded = loaded, .effectReferences = effectReferences};
          registry::readFields(root, configFields(), loaded, context);
          warnScrollButtonBinds(loaded);
          validateEffectReferences(loaded, effectReferences);
        }

        // Reject config if any error-level diagnostics were emitted.
        const bool hasErrors = std::ranges::any_of(configStore().diagnostics(), [](const ConfigDiagnostic& d) {
          return d.severity == ConfigDiagnostic::Severity::Error;
        });
        if (hasErrors) {
          return drmPolicyRequested ? ConfigParseOutcome::Fatal : ConfigParseOutcome::DefaultsAllowed;
        }

        out = std::move(loaded);
        return ConfigParseOutcome::Loaded;
      } catch (const std::exception& exception) {
        emitDiag(ConfigDiagnostic::Severity::Error, nullptr, std::format("config load error: {}", exception.what()));
      } catch (...) {
        emitDiag(ConfigDiagnostic::Severity::Error, nullptr, "config load error: unknown error");
      }
      return drmPolicyRequested ? ConfigParseOutcome::Fatal : ConfigParseOutcome::DefaultsAllowed;
    }

  } // namespace
  const Config::Input::Device* Config::Input::findDevice(std::string_view name) const {
    const auto found = std::ranges::find_if(devices, [name](const Device& device) { return device.name == name; });
    return found == devices.end() ? nullptr : &*found;
  }

  registry::Descriptions registry::describeConfig() {
    Descriptions keys;
    describeFields(configFields(), Config{}, "", keys);
    return keys;
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

  bool ConfigStore::load(const char* explicitPath) {
    ConfigSelection selection;
    if (explicitPath != nullptr) {
      m_implicitCandidates.clear();
      selection.root = std::filesystem::path(explicitPath);
      selection.watchPaths.push_back(selection.root);
    } else {
      m_implicitCandidates = defaultConfigCandidates();
      selection = selectDefaultConfig(m_implicitCandidates);
    }
    setRootPath(selection.root, explicitPath != nullptr);

    Config loaded;
    loaded.keybinds = defaultKeybinds();
    const ConfigParseOutcome outcome = parseInto(loaded, selection.root, selection.watchPaths);
    const bool missing = outcome == ConfigParseOutcome::Missing;
    if (missing) {
      if (m_explicitPath) {
        emitDiag(
            ConfigDiagnostic::Severity::Error, nullptr,
            std::format("config file not found: {}", selection.root.string())
        );
      } else {
        kLog.info("no config file found: {}, using defaults", selection.root.string());
      }
    }
    sortDiagnostics();
    if (outcome == ConfigParseOutcome::Fatal || (missing && m_explicitPath)) {
      return false;
    }
    (void)commit(std::move(loaded), selection.root, missing);
    return true;
  }

  ConfigReloadResult ConfigStore::reload() {
    ConfigSelection selection;
    if (m_explicitPath) {
      selection.root = m_rootPath;
      selection.watchPaths.push_back(selection.root);
    } else {
      selection = selectDefaultConfig(m_implicitCandidates);
    }

    Config loaded;
    loaded.keybinds = defaultKeybinds();
    const ConfigParseOutcome outcome = parseInto(loaded, selection.root, selection.watchPaths);
    const bool missing = outcome == ConfigParseOutcome::Missing;
    if (m_explicitPath && missing) {
      emitDiag(
          ConfigDiagnostic::Severity::Error, nullptr, std::format("config file not found: {}", selection.root.string())
      );
    }
    sortDiagnostics();
    if (outcome != ConfigParseOutcome::Loaded) {
      if (!m_explicitPath && !selection.found && missing) {
        kLog.info("no config file found: {}, using defaults", selection.root.string());
        return commit(std::move(loaded), selection.root, true);
      }
      kLog.warn("config reload failed; keeping previous configuration");
      return {};
    }
    return commit(std::move(loaded), selection.root, false);
  }

  bool loadConfig(const char* explicitPath) { return configStore().load(explicitPath); }

  ConfigReloadResult reloadConfig() { return configStore().reload(); }

  const std::vector<std::filesystem::path>& configWatchPaths() { return configStore().watchPaths(); }

} // namespace umbriel
