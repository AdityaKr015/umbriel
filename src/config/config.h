#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace umbriel {
  enum class KeybindAction {
    None,
    Spawn,
    SpawnTerminal,
    Close,
    Quit,
    FocusLeft,
    FocusRight,
    FocusUp,
    FocusDown,
    MoveColumnLeft,
    MoveColumnRight,
    MoveUp,
    MoveDown,
    ConsumeLeft,
    ExpelRight,
    CycleWidth,
    ToggleFullWidth,
    FocusNext,
    Workspace,
    MoveToWorkspace,
  };

  struct Keybind {
    uint32_t modifiers = 0;
    bool useMod = false;
    uint32_t keysym = 0;
    KeybindAction action = KeybindAction::None;
    std::string spawnCommand;
    int workspace = 0;
  };

  struct Config {
    struct Appearance {
      int borderWidth = 2;
      int cornerRadius = 10;
      std::array<float, 4> borderFocused{0.48F, 0.64F, 1.0F, 1.0F};
      std::array<float, 4> borderUnfocused{0.16F, 0.16F, 0.20F, 1.0F};
      int animationMs = 250;
    } appearance;

    struct Layout {
      int gap = 8;
      double defaultWidthFraction = 0.5;
      std::vector<double> widthPresets{1.0 / 3, 0.5, 2.0 / 3};
      int scrollWheelStep = 60;
    } layout;

    struct General {
      std::string terminal;
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
    } input;

    std::vector<Keybind> keybinds;
  };

  [[nodiscard]] const Config& config();
  void loadConfig(const char* explicitPath);

} // namespace umbriel
