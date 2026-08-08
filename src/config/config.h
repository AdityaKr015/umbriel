#pragma once

#include "config/config_diag.h"
#include "layout/layout.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

namespace umbriel {
  enum class WheelDirection {
    None,
    Up,
    Down,
    Left,
    Right,
  };

  enum class KeybindAction {
    None,
    Spawn,
    WindowClose,
    SessionQuit,
    WindowFocusLeft,
    WindowFocusRight,
    WindowFocusUp,
    WindowFocusDown,
    ColumnMoveLeft,
    ColumnMoveRight,
    WindowMoveUp,
    WindowMoveDown,
    WindowConsumeLeft,
    WindowExpelRight,
    WindowCycleWidth,
    WindowSetWidth,
    ToggleMaximize,
    ToggleFullscreen,
    ToggleFloating,
    WindowFocusNext,
    WorkspaceSwitch,
    WindowMoveToWorkspace,
    ConfigReload,
    LayoutScrollLeft,
    LayoutScrollRight,
    OverviewToggle,
    OverviewOpen,
    OverviewClose,
  };

  struct Keybind {
    uint32_t modifiers = 0;
    bool useMod = false;
    uint32_t keysym = 0;
    WheelDirection wheel = WheelDirection::None;
    uint32_t mouseButton = 0; // evdev BTN_* code, 0 = not a mouse bind
    KeybindAction action = KeybindAction::None;
    std::string spawnCommand;
    std::string workspaceName;
    std::string workspaceOutput;
    double widthFraction = 0.0;
    bool repeat = true;
  };

  // Per-workspace layout overrides (all optional → inherit Config::Layout).
  struct WorkspaceLayoutOverrides {
    std::optional<LayoutMode> mode;
    std::optional<int> gap;
    std::optional<double> defaultWidthFraction;
    std::optional<std::vector<double>> widthPresets;
  };

  // Layout rule parsed from a [[workspace]] entry. Exactly one selector is set.
  struct WorkspaceConfig {
    std::string name;
    std::string output;       // optional output selector
    std::optional<int> index; // optional 1-based position selector
    WorkspaceLayoutOverrides layout;
  };

  // Fully resolved layout config — no optionals. Owned by each Workspace.
  struct ResolvedLayoutConfig {
    LayoutMode mode = LayoutMode::Scrolling;
    int gap = 8;
    double defaultWidthFraction = 0.5;
    std::vector<double> widthPresets{1.0 / 3, 0.5, 2.0 / 3};
    // Derived from gap + appearance border widths; set by resolve function.
    int totalGap = 0; // gap + 2 * totalBorderWidth
    int edgePad = 0;  // gap + totalBorderWidth
  };

  // Resolved workspace entry for a specific output (name + layout config).
  struct ResolvedWorkspace {
    std::string name;
    ResolvedLayoutConfig layout;
  };
  struct OutputMode {
    int width = 0;
    int height = 0;
    int refreshMHz = 0;
  };

  struct OutputRule {
    std::string name;
    std::optional<OutputMode> mode;
    std::optional<std::array<int, 2>> position;
    std::optional<double> scale;
    std::optional<int> transform;
    // Explicit workspace inventory. Omitted means nine numeric workspaces.
    std::optional<std::vector<std::string>> workspaces;
  };

  struct WindowRule {
    std::string appIdPattern;
    std::string titlePattern;
    std::regex appIdRegex;
    std::regex titleRegex;
    std::optional<std::string> defaultOutput;
    std::optional<bool> defaultFloating;
    std::optional<std::array<int, 2>> defaultSize; // [width, height]
    std::optional<double> defaultWidth;            // column width fraction override
    std::optional<int> defaultWorkspace;           // 1-9
    std::optional<bool> defaultFullscreen;
    std::optional<bool> defaultMaximize;
    std::optional<double> opacity; // 0.0-1.0
  };

  // Resolved result: merge of all matching rules (last writer wins per field).
  struct ResolvedWindowRule {
    std::optional<std::string> defaultOutput;
    std::optional<bool> defaultFloating;
    std::optional<std::array<int, 2>> defaultSize;
    std::optional<double> defaultWidth;
    std::optional<int> defaultWorkspace;
    std::optional<bool> defaultFullscreen;
    std::optional<bool> defaultMaximize;
    std::optional<double> opacity;
  };

  struct LayerRule {
    std::string namespacePattern;
    std::regex namespaceRegex;
    std::optional<bool> blur;
    std::optional<double> ignoreAlpha;
    std::optional<bool> optimized;
  };

  struct ResolvedLayerRule {
    std::optional<bool> blur;
    std::optional<double> ignoreAlpha;
    std::optional<bool> optimized;
  };

  struct Config {
    struct Appearance {
      int borderWidth = 2;
      int outerBorderWidth = 0;
      int cornerRadius = 10;
      std::array<float, 4> borderFocused{0.48F, 0.64F, 1.0F, 1.0F};
      std::array<float, 4> borderUnfocused{0.16F, 0.16F, 0.20F, 1.0F};
      std::array<float, 4> outerBorderColor{0.10F, 0.10F, 0.12F, 1.0F};
      std::array<float, 4> insertHintColor{0.50F, 0.78F, 1.0F, 0.50F};
      std::array<float, 4> backdropColor{0.0F, 0.0F, 0.0F, 1.0F};
      int animationMs = 250;
      struct Blur {
        bool enabled = true;
        bool optimized = true;
        int passes = 3;
        int radius = 5;
        double noise = 0.02;
        double brightness = 0.9;
        double contrast = 0.9;
        double saturation = 1.1;
      } blur;
      struct Shadow {
        bool enabled = true;
        int softness = 10;
        int offsetX = 2;
        int offsetY = 2;
        std::array<float, 4> color{0.0F, 0.0F, 0.0F, 0.55F};
      } shadow;
      bool preferNoCsd = true;

      [[nodiscard]] int totalBorderWidth() const { return borderWidth + outerBorderWidth; }
    } appearance;

    struct Overview {
      // Workspace scale when fully zoomed out (niri's compute_overview_zoom).
      double zoom = 0.5;
      // Tint laid over the desktop background while the overview is up. The
      // wallpaper (background layer) is never hidden by anything else, so a
      // transparent default leaves it untouched; raise the alpha to dim it,
      // FF to replace it outright.
      std::array<float, 4> backdropColor{0.0627451F, 0.0627451F, 0.0784314F, 0.0F};
      std::array<float, 4> workspaceColor{0.1019608F, 0.1019608F, 0.1215686F, 1.0F};
    } overview;

    struct Layout {
      LayoutMode mode = LayoutMode::Scrolling;
      int gap = 8;
      double defaultWidthFraction = 0.5;
      std::vector<double> widthPresets{1.0 / 3, 0.5, 2.0 / 3};
    } layout;

    // Clear `layout.gap` outside decoration edges: borders are drawn outside the
    // surface, so tile spacing and usable-area insets include total border width.
    [[nodiscard]] int layoutGap() const { return layout.gap + 2 * appearance.totalBorderWidth(); }
    [[nodiscard]] int layoutEdgePad() const { return layout.gap + appearance.totalBorderWidth(); }

    struct Workspaces {
      // Re-selecting the active workspace jumps back to the previous one.
      bool backAndForth = false;
    } workspaces;

    struct General {
      std::vector<std::string> autostart;
      // Spawn and manage xwayland-satellite for X11 app support. Requires restart.
      bool xwayland = true;
    } general;

    struct Input {
      struct Keyboard {
        std::string layout;
        std::string variant;
        int repeatRate = 25;
        int repeatDelay = 600;
      } keyboard;

      struct Touchpad {
        std::optional<bool> tap;
        std::optional<bool> naturalScroll;
      } touchpad;

      struct Mouse {
        std::optional<bool> naturalScroll;
        int scrollWheelStep = 60;
      } mouse;

      struct Cursor {
        std::string theme;
        int size = 24;
      } cursor;

      struct Focus {
        bool followsMouse = false;
        std::optional<double> followsMouseMaxScroll;
      } focus;
    } input;

    std::vector<Keybind> keybinds;
    std::vector<OutputRule> outputs;
    std::vector<WindowRule> windowRules;
    std::vector<LayerRule> layerRules;
    std::vector<WorkspaceConfig> workspaceRules; // [[workspace]] layout rules
  };

  [[nodiscard]] const Config& config();
  void loadConfig(const char* explicitPath);
  [[nodiscard]] bool reloadConfig();
  [[nodiscard]] const std::vector<std::filesystem::path>& configWatchPaths();
  [[nodiscard]] const std::vector<ConfigDiagnostic>& configDiagnostics();
  [[nodiscard]] const std::filesystem::path& configRootPath();
  [[nodiscard]] ResolvedWindowRule resolveWindowRules(const char* appId, const char* title);
  [[nodiscard]] ResolvedLayerRule resolveLayerRules(const char* layerNamespace);
  [[nodiscard]] bool anyWindowRuleHasTitlePattern();
  [[nodiscard]] ResolvedLayoutConfig resolveGlobalLayout();
  [[nodiscard]] std::vector<ResolvedWorkspace> resolveWorkspacesForOutput(const char* outputName);

  enum class ActionArgKind : uint8_t { None, Command, WidthFraction, Workspace };

  struct ActionSpec {
    std::string_view name;  // e.g. "spawn", "workspace-switch", "window-close"
    std::string_view param; // "" for simple, "<cmd>" / "<workspace>[/<output>]" for parameterized
    KeybindAction action;
    ActionArgKind argKind = ActionArgKind::None;
  };

  bool parseAction(std::string_view value, Keybind& output);
  std::span<const ActionSpec> actionSpecs();
} // namespace umbriel
