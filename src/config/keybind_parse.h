#pragma once

// Keybind vocabulary and the pure text-to-struct parsers over it. Split out of
// config.h so the parsing can be exercised without loading a config file.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
    TogglePinned,
    WindowFocusNext,
    WorkspaceSwitch,
    WindowMoveToWorkspace,
    ConfigReload,
    LayoutScrollLeft,
    LayoutScrollRight,
    OverviewToggle,
    OverviewOpen,
    OverviewClose,
    CheatsheetToggle,
    CheatsheetOpen,
    CheatsheetClose,
    WindowMoveToScratchpad,
    ScratchpadToggle,
    WindowRestoreFromScratchpad,
    ScratchpadFocusNext,
    Submap,
  };

  struct Keybind {
    std::string submap;
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
    std::string scratchpadOutput;
  };

  enum class ActionArgKind : uint8_t { None, Command, WidthFraction, Workspace, OptionalOutput };

  struct ActionSpec {
    std::string_view name;  // e.g. "spawn", "workspace-switch", "window-close"
    std::string_view param; // "" for simple, "<cmd>" / "<workspace>[/<output>]" for parameterized
    KeybindAction action;
    ActionArgKind argKind = ActionArgKind::None;
  };

  // Parse a chord such as "Mod+Shift+h", "Ctrl+Alt+Delete", "Mod+WheelUp",
  // "Mod+MouseBack", or "submap[resize],Escape". Only the trigger fields are
  // written; the action is set separately by parseAction. Returns false and
  // leaves `output` default-constructed on any malformed input.
  bool parseChord(std::string_view chord, Keybind& output);

  // Parse an action such as "window-close", "spawn:foot", "window-set-width:0.5",
  // or "workspace-switch:2/DP-1", writing the action and its payload into
  // `output` without touching the trigger fields.
  bool parseAction(std::string_view value, Keybind& output);

  std::span<const ActionSpec> actionSpecs();

  // The binds used when no config file supplies any.
  std::vector<Keybind> defaultKeybinds();

} // namespace umbriel
