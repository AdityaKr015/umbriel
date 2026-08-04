#pragma once

#include "config/config_diag.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
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
    TerminalSpawn,
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
    ToggleMaximize,
    ToggleFullscreen,
    ToggleFloating,
    WindowFocusNext,
    WorkspaceSwitch,
    WindowMoveToWorkspace,
    ConfigReload,
    LayoutScrollLeft,
    LayoutScrollRight,
  };

  struct Keybind {
    uint32_t modifiers = 0;
    bool useMod = false;
    uint32_t keysym = 0;
    WheelDirection wheel = WheelDirection::None;
    KeybindAction action = KeybindAction::None;
    std::string spawnCommand;
    int workspace = 0;
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
      int animationMs = 250;
      struct Blur {
        bool enabled = true;
        int passes = 3;
        int radius = 5;
        double noise = 0.02;
        double brightness = 0.9;
        double contrast = 0.9;
        double saturation = 1.1;
        double ignoreAlpha = 0.5;
      } blur;

      [[nodiscard]] int totalBorderWidth() const { return borderWidth + outerBorderWidth; }
    } appearance;

    struct Layout {
      int gap = 8;
      double defaultWidthFraction = 0.5;
      std::vector<double> widthPresets{1.0 / 3, 0.5, 2.0 / 3};
      int scrollWheelStep = 60;
    } layout;

    // Clear `layout.gap` outside decoration edges: borders are drawn outside the
    // surface, so tile spacing and usable-area insets include total border width.
    [[nodiscard]] int layoutGap() const { return layout.gap + 2 * appearance.totalBorderWidth(); }
    [[nodiscard]] int layoutEdgePad() const { return layout.gap + appearance.totalBorderWidth(); }

    struct General {
      std::string terminal;
      std::vector<std::string> autostart;
      bool preferNoCsd = false;
      // Re-selecting the active workspace jumps back to the previous one.
      bool workspaceBackAndForth = false;
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
  };

  [[nodiscard]] const Config& config();
  void loadConfig(const char* explicitPath);
  [[nodiscard]] bool reloadConfig();
  [[nodiscard]] const std::vector<std::filesystem::path>& configWatchPaths();
  [[nodiscard]] const std::vector<ConfigDiagnostic>& configDiagnostics();
  [[nodiscard]] const std::filesystem::path& configRootPath();

} // namespace umbriel
