#pragma once

// Cheatsheet content, independent of how it is drawn.
//
// Turning the configured keybinds into display rows is where the interesting
// logic lives: merging binds that share an action, marking repeats with a ditto,
// splitting a spawn command into binary and arguments, and collapsing the
// per-digit workspace binds into one row. None of that needs pango, cairo, or a
// running compositor, so it lives here and is tested directly.

#include "config/keybind_parse.h"

#include <span>
#include <string>
#include <vector>

namespace umbriel {

  struct CheatsheetRow {
    std::string chord;  // display chord(s)
    std::string action; // display action (full label for non-spawn, args-only for spawn)
    KeybindAction actionType = KeybindAction::None;
    std::string submap;      // source submap (empty = top-level)
    std::string spawnBinary; // basename of spawn command (empty for non-spawn)
    std::string spawnArgs;   // args portion of spawn command
    // For workspace collapse detection.
    uint32_t keysym = 0;
    std::string workspaceName;
    uint32_t modifiers = 0;
    bool useMod = false;
  };

  // Group assignment.
  enum class Group : int {
    Apps = 0,
    Focus,
    MoveSize,
    Windows,
    Workspaces,
    Overview,
    System,
    SubmapBase = 100, // submaps start here
  };

  [[nodiscard]] const char* groupTitle(Group group);
  [[nodiscard]] Group groupForAction(KeybindAction action);

  // One row per chord. Binds sharing an action collapse into a group whose first
  // row carries the label and whose others carry a ditto mark.
  [[nodiscard]] std::vector<CheatsheetRow> buildCheatsheetRows(std::span<const Keybind> keybinds);

} // namespace umbriel
