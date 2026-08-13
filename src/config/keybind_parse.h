#pragma once

// Keybind vocabulary and the pure text-to-struct parsers over it. Split out of
// config.h so the parsing can be exercised without loading a config file.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
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

  // Action payloads. Exactly one is valid for a given action, so they live in a
  // variant rather than as sibling fields: a spawn command and a workspace
  // selector can no longer be set at the same time, and the submap name no
  // longer shares storage with the spawn command.
  struct SpawnArg {
    std::string command;
    bool operator==(const SpawnArg&) const = default;
  };
  struct SubmapArg {
    std::string name;
    bool operator==(const SubmapArg&) const = default;
  };
  struct WidthArg {
    double fraction = 0.0;
    bool operator==(const WidthArg&) const = default;
  };
  struct WorkspaceArg {
    std::string name;
    std::string output; // empty = resolve against the focused output
    bool operator==(const WorkspaceArg&) const = default;
  };
  struct OutputArg {
    std::string output; // empty = the focused output
    bool operator==(const OutputArg&) const = default;
  };

  using KeybindPayload = std::variant<std::monostate, SpawnArg, SubmapArg, WidthArg, WorkspaceArg, OutputArg>;

  struct Keybind {
    // What triggers the bind.
    std::string submap;
    uint32_t modifiers = 0;
    bool useMod = false;
    uint32_t keysym = 0;
    WheelDirection wheel = WheelDirection::None;
    uint32_t mouseButton = 0; // evdev BTN_* code, 0 = not a mouse bind
    bool repeat = true;

    // What it does.
    KeybindAction action = KeybindAction::None;
    KeybindPayload payload;

    bool operator==(const Keybind&) const = default;
  };

  // Null unless the bind carries that payload alternative.
  template <typename Arg> [[nodiscard]] const Arg* payloadIf(const Keybind& bind) {
    return std::get_if<Arg>(&bind.payload);
  }

  // "reset" and "disable" pop the current submap instead of pushing a new one.
  // Recognised in two places: the action itself, and the keybind matcher, which
  // lets such a bind fire from inside any submap as an emergency exit.
  [[nodiscard]] inline bool isSubmapReset(const SubmapArg& arg) { return arg.name == "reset" || arg.name == "disable"; }

  [[nodiscard]] inline bool isSubmapResetBind(const Keybind& bind) {
    const auto* arg = payloadIf<SubmapArg>(bind);
    return bind.action == KeybindAction::Submap && arg != nullptr && isSubmapReset(*arg);
  }

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
