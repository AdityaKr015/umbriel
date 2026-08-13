#include "scene/cheatsheet_rows.h"

#include "check.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include <xkbcommon/xkbcommon.h>
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}
// clang-format on

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using umbriel::buildCheatsheetRows;
using umbriel::CheatsheetRow;
using umbriel::Keybind;
using umbriel::KeybindAction;

namespace {

  Keybind bind(KeybindAction action, uint32_t keysym, uint32_t modifiers = 0) {
    Keybind result;
    result.useMod = true;
    result.modifiers = modifiers;
    result.keysym = keysym;
    result.action = action;
    return result;
  }

  Keybind spawnBind(uint32_t keysym, const std::string& command) {
    Keybind result = bind(KeybindAction::Spawn, keysym);
    umbriel::SpawnArg spawn;
    spawn.command = command;
    result.payload = std::move(spawn);
    return result;
  }

  Keybind workspaceBind(uint32_t keysym, const std::string& name, uint32_t modifiers = 0) {
    Keybind result = bind(KeybindAction::WorkspaceSwitch, keysym, modifiers);
    umbriel::WorkspaceArg workspace;
    workspace.name = name;
    result.payload = std::move(workspace);
    return result;
  }

  // The ditto mark a merged group's second and later rows carry.
  const std::string kDitto = "\xe2\x80\xb3";

  size_t countRows(const std::vector<CheatsheetRow>& rows, KeybindAction action) {
    return static_cast<size_t>(std::ranges::count_if(rows, [action](const CheatsheetRow& row) {
      return row.actionType == action;
    }));
  }

} // namespace

UMBRIEL_TEST(anEmptyBindListProducesNoRows) { CHECK(buildCheatsheetRows({}).empty()); }

UMBRIEL_TEST(bindsWithNoActionAreSkipped) {
  const std::vector<Keybind> binds = {Keybind{}, bind(KeybindAction::WindowClose, XKB_KEY_q)};
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  CHECK(rows[0].actionType == KeybindAction::WindowClose);
}

UMBRIEL_TEST(oneRowPerBindWhenActionsDiffer) {
  const std::vector<Keybind> binds = {
      bind(KeybindAction::WindowClose, XKB_KEY_q),
      bind(KeybindAction::ToggleFloating, XKB_KEY_t),
      bind(KeybindAction::OverviewToggle, XKB_KEY_o),
  };
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{3});
  for (const CheatsheetRow& row : rows) {
    CHECK(row.action != kDitto);
    CHECK(!row.chord.empty());
  }
}

UMBRIEL_TEST(bindsSharingAnActionMergeWithADitto) {
  // Mod+H and Mod+Left both focus left; the second row shows the chord and a
  // ditto rather than repeating the label.
  const std::vector<Keybind> binds = {
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_h),
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_Left),
  };
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{2});
  CHECK(rows[0].action != kDitto);
  CHECK(!rows[0].action.empty());
  CHECK_EQ(rows[1].action, kDitto);
  CHECK(rows[0].chord != rows[1].chord);
}

UMBRIEL_TEST(differentModifiersDoNotMerge) {
  // Same action, different modifier: these are distinct entries, not a repeat.
  const std::vector<Keybind> binds = {
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_h),
      bind(KeybindAction::WindowFocusLeft, XKB_KEY_h, WLR_MODIFIER_SHIFT),
  };
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{2});
  CHECK(rows[0].action != kDitto);
  CHECK(rows[1].action != kDitto);
}

UMBRIEL_TEST(spawnIsSplitIntoBinaryAndArguments) {
  const std::vector<Keybind> binds = {spawnBind(XKB_KEY_Return, "/usr/bin/foot -e htop")};
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  // The binary is shown separately so several spawns can group under it, and
  // the row itself carries the arguments.
  CHECK_EQ(rows[0].spawnBinary, std::string{"foot"});
  CHECK_EQ(rows[0].spawnArgs, std::string{"-e htop"});
}

UMBRIEL_TEST(spawnWithNoArgumentsFallsBackToTheBinary) {
  const std::vector<Keybind> binds = {spawnBind(XKB_KEY_Return, "foot")};
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{1});
  CHECK_EQ(rows[0].spawnBinary, std::string{"foot"});
  CHECK(rows[0].spawnArgs.empty());
  CHECK_EQ(rows[0].action, std::string{"foot"});
}

UMBRIEL_TEST(spawnsWithDifferentCommandsDoNotMerge) {
  const std::vector<Keybind> binds = {
      spawnBind(XKB_KEY_Return, "foot"),
      spawnBind(XKB_KEY_b, "firefox"),
  };
  CHECK_EQ(buildCheatsheetRows(binds).size(), size_t{2});
}

UMBRIEL_TEST(perDigitWorkspaceBindsCollapseToOneRow) {
  // The defaults bind workspaces 1-9 on both the number row and the keypad.
  // Eighteen rows of near-identical text would swamp the sheet, so they
  // collapse to a single "1…9" row.
  std::vector<Keybind> binds;
  for (int i = 0; i < 9; ++i) {
    const auto digit = static_cast<uint32_t>(XKB_KEY_1 + i);
    const auto keypad = static_cast<uint32_t>(XKB_KEY_KP_1 + i);
    binds.push_back(workspaceBind(digit, std::to_string(i + 1)));
    binds.push_back(workspaceBind(keypad, std::to_string(i + 1)));
  }

  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(countRows(rows, KeybindAction::WorkspaceSwitch), size_t{1});
  CHECK(rows.size() == 1);
  if (!rows.empty()) {
    CHECK(rows[0].chord.contains("1"));
    CHECK(rows[0].action.contains("1-9"));
  }
}

UMBRIEL_TEST(anIncompleteWorkspaceRunIsLeftAlone) {
  // Only three digits bound: collapsing would misreport the range, so the rows
  // stay as they are.
  std::vector<Keybind> binds;
  binds.reserve(3);
  for (int i = 0; i < 3; ++i) {
    binds.push_back(workspaceBind(static_cast<uint32_t>(XKB_KEY_1 + i), std::to_string(i + 1)));
  }
  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(countRows(rows, KeybindAction::WorkspaceSwitch), size_t{3});
}

UMBRIEL_TEST(submapBindsCarryTheirSubmap) {
  Keybind inSubmap = bind(KeybindAction::WindowFocusLeft, XKB_KEY_h);
  inSubmap.submap = "resize";
  const std::vector<Keybind> binds = {bind(KeybindAction::WindowFocusLeft, XKB_KEY_h), inSubmap};

  const auto rows = buildCheatsheetRows(binds);
  CHECK_EQ(rows.size(), size_t{2});
  // A submap bind never merges with the same action at top level.
  const bool haveTopLevel = std::ranges::any_of(rows, [](const CheatsheetRow& r) { return r.submap.empty(); });
  const bool haveSubmap = std::ranges::any_of(rows, [](const CheatsheetRow& r) { return r.submap == "resize"; });
  CHECK(haveTopLevel);
  CHECK(haveSubmap);
}

UMBRIEL_TEST(everyActionMapsToAGroupWithATitle) {
  // groupForAction has no default arm to fall through to, so a new action that
  // is never grouped would show up here.
  for (const auto& spec : umbriel::actionSpecs()) {
    const umbriel::Group group = umbriel::groupForAction(spec.action);
    const char* title = umbriel::groupTitle(group);
    CHECK(title != nullptr);
    CHECK(title != nullptr && title[0] != '\0');
  }
}

int main() { return RUN_TESTS(); }
