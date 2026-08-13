#include "scene/cheatsheet.h"

#include "config/config.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

  constexpr int kPad = 28;
  constexpr int kCornerRadius = 16;
  constexpr int kColumnGap = 32;
  constexpr int kTitleBodyGap = 12;
  constexpr int kBodyFooterGap = 12;
  constexpr int kColumnMaxWidth = 600;
  constexpr int kMaxColumns = 4;
  constexpr float kPanelColor[] = {0.078F, 0.078F, 0.098F, 0.94F};

  // --- Chord reconstruction helpers ---

  const char* wheelName(umbriel::WheelDirection dir) {
    switch (dir) {
    case umbriel::WheelDirection::Up:
      return "WheelUp";
    case umbriel::WheelDirection::Down:
      return "WheelDown";
    case umbriel::WheelDirection::Left:
      return "WheelLeft";
    case umbriel::WheelDirection::Right:
      return "WheelRight";
    default:
      return nullptr;
    }
  }

  const char* mouseButtonName(uint32_t button) {
    switch (button) {
    case BTN_LEFT:
      return "MouseLeft";
    case BTN_RIGHT:
      return "MouseRight";
    case BTN_MIDDLE:
      return "MouseMiddle";
    case BTN_SIDE:
      return "MouseBack";
    case BTN_EXTRA:
      return "MouseForward";
    default:
      return nullptr;
    }
  }

  std::string prettifyKeysym(const char* raw) {
    // Single lowercase ASCII letter -> uppercase.
    if (raw[0] != '\0' && raw[1] == '\0' && raw[0] >= 'a' && raw[0] <= 'z') {
      return std::string(1, static_cast<char>(raw[0] - 'a' + 'A'));
    }

    struct Alias {
      const char* from;
      const char* to;
    };
    static constexpr Alias kAliases[] = {
        {"Left", "\xe2\x86\x90"},  // ←
        {"Right", "\xe2\x86\x92"}, // →
        {"Up", "\xe2\x86\x91"},    // ↑
        {"Down", "\xe2\x86\x93"},  // ↓
        {"comma", ","},
        {"period", "."},
        {"slash", "/"},
        {"minus", "-"},
        {"equal", "="},
        {"semicolon", ";"},
        {"apostrophe", "'"},
        {"grave", "`"},
        {"bracketleft", "["},
        {"bracketright", "]"},
        {"backslash", "\\"},
        {"space", "Space"},
        {"Prior", "PgUp"},
        {"Next", "PgDn"},
    };
    for (const auto& alias : kAliases) {
      if (std::string_view(raw) == alias.from) {
        return alias.to;
      }
    }
    return raw;
  }

  std::string buildChordLabel(const umbriel::Keybind& bind) {
    std::string result;
    auto appendMod = [&](const char* name) {
      if (!result.empty()) {
        result += '+';
      }
      result += name;
    };
    if (bind.useMod) {
      appendMod("Mod");
    }
    if ((bind.modifiers & WLR_MODIFIER_LOGO) != 0) {
      appendMod("Super");
    }
    if ((bind.modifiers & WLR_MODIFIER_CTRL) != 0) {
      appendMod("Ctrl");
    }
    if ((bind.modifiers & WLR_MODIFIER_ALT) != 0) {
      appendMod("Alt");
    }
    if ((bind.modifiers & WLR_MODIFIER_SHIFT) != 0) {
      appendMod("Shift");
    }

    if (!result.empty()) {
      result += '+';
    }
    if (bind.wheel != umbriel::WheelDirection::None) {
      const char* name = wheelName(bind.wheel);
      result += name != nullptr ? name : "Wheel?";
    } else if (bind.mouseButton != 0) {
      const char* name = mouseButtonName(bind.mouseButton);
      result += name != nullptr ? name : "Mouse?";
    } else {
      char buf[64];
      xkb_keysym_get_name(bind.keysym, buf, sizeof(buf));
      result += prettifyKeysym(buf);
    }
    return result;
  }

  // --- Action label ---

  // Decompose a spawn command into binary basename and args for compact display.
  // IPC pattern "<binary> msg <args>" collapses the "msg" boilerplate.
  struct SpawnParts {
    std::string binary; // basename of the executable
    std::string args;   // arguments (empty for single-word commands)
  };

  SpawnParts splitSpawnCommand(const std::string& cmd) {
    if (cmd.empty()) {
      return {"", ""};
    }
    std::string_view view(cmd);
    const size_t firstSpace = view.find(' ');
    std::string_view binPath = view.substr(0, firstSpace);
    const size_t slash = binPath.rfind('/');
    std::string base(slash != std::string_view::npos ? binPath.substr(slash + 1) : binPath);
    if (firstSpace == std::string_view::npos) {
      return {std::move(base), ""};
    }
    std::string_view rest = view.substr(firstSpace + 1);
    // Collapse "<binary> msg <args>" -> args only.
    if (rest.starts_with("msg ") && rest.size() > 4) {
      return {std::move(base), std::string(rest.substr(4))};
    }
    return {std::move(base), std::string(rest)};
  }

  // Empty unless the bind targets a workspace. Used to collapse the runs of
  // per-digit workspace binds into a single row.
  std::string workspaceSelectorName(const umbriel::Keybind& bind) {
    const auto* workspace = umbriel::payloadIf<umbriel::WorkspaceArg>(bind);
    return workspace != nullptr ? workspace->name : std::string{};
  }

  // Driven by the spec's argument kind and the bind's payload variant, so the
  // set of parameterized actions lives in exactly one place: the spec table.
  std::string actionLabel(const umbriel::Keybind& bind) {
    for (const auto& spec : umbriel::actionSpecs()) {
      if (spec.action != bind.action) {
        continue;
      }
      std::string name(spec.name);
      switch (spec.argKind) {
      case umbriel::ActionArgKind::None:
        return name;
      case umbriel::ActionArgKind::Command:
        if (const auto* spawn = umbriel::payloadIf<umbriel::SpawnArg>(bind)) {
          return name + ": " + spawn->command;
        }
        if (const auto* submap = umbriel::payloadIf<umbriel::SubmapArg>(bind)) {
          return name + ": " + submap->name;
        }
        return name;
      case umbriel::ActionArgKind::WidthFraction:
        if (const auto* width = umbriel::payloadIf<umbriel::WidthArg>(bind)) {
          return std::format("{}: {:.2g}", name, width->fraction);
        }
        return name;
      case umbriel::ActionArgKind::Workspace:
        if (const auto* workspace = umbriel::payloadIf<umbriel::WorkspaceArg>(bind)) {
          std::string label = name + ": " + workspace->name;
          if (!workspace->output.empty()) {
            label += "/" + workspace->output;
          }
          return label;
        }
        return name;
      case umbriel::ActionArgKind::OptionalOutput:
        if (const auto* output = umbriel::payloadIf<umbriel::OutputArg>(bind);
            output != nullptr && !output->output.empty()) {
          return name + ": " + output->output;
        }
        return name;
      }
      return name;
    }
    return "unknown";
  }

  // --- Row data ---

  struct CheatsheetRow {
    std::string chord;  // display chord(s)
    std::string action; // display action (full label for non-spawn, args-only for spawn)
    umbriel::KeybindAction actionType = umbriel::KeybindAction::None;
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

  const char* groupTitle(Group group) {
    switch (group) {
    case Group::Apps:
      return "Apps";
    case Group::Focus:
      return "Focus";
    case Group::MoveSize:
      return "Move &amp; size";
    case Group::Windows:
      return "Windows";
    case Group::Workspaces:
      return "Workspaces";
    case Group::Overview:
      return "Overview";
    case Group::System:
      return "System";
    default:
      return nullptr; // submaps handled separately
    }
  }

  Group groupForAction(umbriel::KeybindAction action) {
    using A = umbriel::KeybindAction;
    switch (action) {
    case A::Spawn:
      return Group::Apps;
    case A::WindowFocusLeft:
    case A::WindowFocusRight:
    case A::WindowFocusUp:
    case A::WindowFocusDown:
    case A::WindowFocusNext:
      return Group::Focus;
    case A::ColumnMoveLeft:
    case A::ColumnMoveRight:
    case A::WindowMoveUp:
    case A::WindowMoveDown:
    case A::WindowConsumeLeft:
    case A::WindowExpelRight:
    case A::WindowCycleWidth:
    case A::WindowSetWidth:
    case A::LayoutScrollLeft:
    case A::LayoutScrollRight:
      return Group::MoveSize;
    case A::WindowClose:
    case A::ToggleFloating:
    case A::ToggleMaximize:
    case A::ToggleFullscreen:
      return Group::Windows;
    case A::WorkspaceSwitch:
    case A::WindowMoveToWorkspace:
      return Group::Workspaces;
    case A::OverviewToggle:
    case A::OverviewOpen:
    case A::OverviewClose:
      return Group::Overview;
    case A::ConfigReload:
    case A::SessionQuit:
    case A::Submap:
    case A::CheatsheetToggle:
    case A::CheatsheetOpen:
    case A::CheatsheetClose:
      return Group::System;
    default:
      return Group::System;
    }
  }

  // Merge key: groups keybinds with identical action for stacked display.
  struct MergeKey {
    std::string submap;
    umbriel::KeybindAction action;
    std::string actionLabel;
    uint32_t modifiers;
    bool useMod;

    bool operator<(const MergeKey& other) const {
      if (submap != other.submap)
        return submap < other.submap;
      if (action != other.action)
        return action < other.action;
      if (actionLabel != other.actionLabel)
        return actionLabel < other.actionLabel;
      if (modifiers != other.modifiers)
        return modifiers < other.modifiers;
      return useMod < other.useMod;
    }
  };

  // Escape text for Pango markup.
  std::string escape(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result(escaped);
    g_free(escaped);
    return result;
  }

} // namespace

namespace umbriel {

  Cheatsheet::Cheatsheet(Server& server, wlr_scene_tree* parent) : m_server(server), m_parent(parent) {}

  Cheatsheet::~Cheatsheet() { hide(); }

  void Cheatsheet::show() {
    if (m_server.sessionLocked()) {
      return;
    }
    render();
  }

  void Cheatsheet::hide() {
    if (m_tree == nullptr) {
      return;
    }
    m_shadow.reset();
    wlr_scene_node_destroy(&m_tree->node);
    m_tree = nullptr;
  }

  void Cheatsheet::toggle() {
    if (m_tree != nullptr) {
      hide();
    } else {
      show();
    }
  }

  bool Cheatsheet::visible() const { return m_tree != nullptr; }

  void Cheatsheet::relayout() {
    if (m_tree != nullptr) {
      render();
    }
  }

  void Cheatsheet::render() {
    // Destroy previous subtree.
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }

    m_tree = wlr_scene_tree_create(m_parent);

    // --- Output info ---
    wlr_output* output = m_server.preferredOutput();
    double scale = 1.0;
    wlr_box outputBox{};
    bool haveOutput = false;
    if (output != nullptr) {
      scale = std::max(1.0, std::ceil(static_cast<double>(output->scale)));
      wlr_output_layout_get_box(m_server.outputLayout(), output, &outputBox);
      haveOutput = true;
    }

    // --- Step 1: Build display rows ---
    const auto& keybinds = config().keybinds;

    // Collect rows, merging keybinds with identical action into one group.
    // Each group becomes multiple display rows: first shows the action,
    // subsequent rows show the chord with a dim ditto mark.
    struct RawRow {
      std::vector<std::string> chords;
      std::string actionStr;
      KeybindAction actionType;
      std::string submap;
      // From the first bind in the group (for workspace collapse).
      uint32_t keysym;
      std::string workspaceName;
      uint32_t modifiers;
      bool useMod;
    };

    std::map<MergeKey, size_t> mergeIndex;
    std::vector<RawRow> rawRows;

    for (const auto& bind : keybinds) {
      if (bind.action == KeybindAction::None) {
        continue;
      }

      std::string chord = buildChordLabel(bind);
      std::string aLabel = actionLabel(bind);

      MergeKey key{
          .submap = bind.submap,
          .action = bind.action,
          .actionLabel = aLabel,
          .modifiers = bind.modifiers,
          .useMod = bind.useMod,
      };

      auto it = mergeIndex.find(key);
      if (it != mergeIndex.end()) {
        rawRows[it->second].chords.push_back(std::move(chord));
      } else {
        mergeIndex[key] = rawRows.size();
        rawRows.push_back({
            .chords = {std::move(chord)},
            .actionStr = std::move(aLabel),
            .actionType = bind.action,
            .submap = bind.submap,
            .keysym = bind.keysym,
            .workspaceName = workspaceSelectorName(bind),
            .modifiers = bind.modifiers,
            .useMod = bind.useMod,
        });
      }
    }

    // Expand merged groups into display rows: first chord gets the action,
    // additional chords get a dim ditto mark (\u2033).
    std::vector<CheatsheetRow> rows;
    rows.reserve(keybinds.size());
    for (auto& raw : rawRows) {
      // For spawn actions, decompose into binary + args for sub-grouped display.
      std::string displayAction = raw.actionStr;
      SpawnParts spawn;
      if (raw.actionType == KeybindAction::Spawn) {
        constexpr std::string_view kPrefix = "spawn: ";
        if (displayAction.starts_with(kPrefix)) {
          spawn = splitSpawnCommand(std::string(displayAction.substr(kPrefix.size())));
          // Display action is the args (or binary if no args).
          displayAction = spawn.args.empty() ? spawn.binary : spawn.args;
        }
      }
      if (displayAction.size() > 32) {
        displayAction = displayAction.substr(0, 32) + "\xe2\x80\xa6"; // …
      }
      for (size_t i = 0; i < raw.chords.size(); ++i) {
        rows.push_back({
            .chord = std::move(raw.chords[i]),
            .action = i == 0 ? displayAction : "\xe2\x80\xb3", // ″ ditto
            .actionType = raw.actionType,
            .submap = raw.submap,
            .spawnBinary = spawn.binary,
            .spawnArgs = spawn.args,
            .keysym = raw.keysym,
            .workspaceName = raw.workspaceName,
            .modifiers = raw.modifiers,
            .useMod = raw.useMod,
        });
      }
    }

    // --- Collapse workspace digit runs ---
    // For each (submap, action in {WorkspaceSwitch, WindowMoveToWorkspace}, modifiers, useMod),
    // check if digits 1..9 are present with workspaceName == digit. If so, collapse them.
    auto collapseWorkspaceRuns = [&](KeybindAction wsAction) {
      struct RunKey {
        std::string submap;
        uint32_t modifiers;
        bool useMod;
        bool operator<(const RunKey& o) const {
          if (submap != o.submap)
            return submap < o.submap;
          if (modifiers != o.modifiers)
            return modifiers < o.modifiers;
          return useMod < o.useMod;
        }
      };
      std::map<RunKey, std::vector<size_t>> groups;
      for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].actionType != wsAction)
          continue;
        if (!rows[i].submap.empty())
          continue; // submaps handled separately
        RunKey rk{rows[i].submap, rows[i].modifiers, rows[i].useMod};
        groups[rk].push_back(i);
      }

      for (auto& [rk, indices] : groups) {
        // Check if we have the full 1..9 digit run.
        std::vector<size_t> digitIndices;
        std::vector<size_t> kpIndices;
        for (size_t idx : indices) {
          const auto& row = rows[idx];
          if (row.keysym >= XKB_KEY_1
              && row.keysym <= XKB_KEY_9
              && row.workspaceName == std::to_string(row.keysym - XKB_KEY_1 + 1)) {
            digitIndices.push_back(idx);
          } else if (row.keysym >= XKB_KEY_KP_1 && row.keysym <= XKB_KEY_KP_9) {
            kpIndices.push_back(idx);
          }
        }
        if (digitIndices.size() < 9) {
          continue; // no full run
        }

        // Build the modifier prefix for the collapsed row.
        std::string modPrefix;
        if (rk.useMod) {
          modPrefix = "Mod";
        }
        if ((rk.modifiers & WLR_MODIFIER_LOGO) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Super";
        }
        if ((rk.modifiers & WLR_MODIFIER_CTRL) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Ctrl";
        }
        if ((rk.modifiers & WLR_MODIFIER_ALT) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Alt";
        }
        if ((rk.modifiers & WLR_MODIFIER_SHIFT) != 0) {
          if (!modPrefix.empty())
            modPrefix += '+';
          modPrefix += "Shift";
        }
        if (!modPrefix.empty())
          modPrefix += '+';

        // Find the action spec name.
        std::string_view specName;
        for (const auto& spec : actionSpecs()) {
          if (spec.action == wsAction) {
            specName = spec.name;
            break;
          }
        }

        // Replace the first digit index row with the collapsed version.
        rows[digitIndices[0]].chord = modPrefix
            + "1\xe2\x80\xa6"
              "9"; // 1…9
        rows[digitIndices[0]].action = std::string(specName) + ": 1-9";

        // Mark remaining digit rows and all KP rows for deletion.
        std::vector<size_t> toRemove;
        for (size_t i = 1; i < digitIndices.size(); ++i) {
          toRemove.push_back(digitIndices[i]);
        }
        for (size_t idx : kpIndices) {
          toRemove.push_back(idx);
        }
        std::ranges::sort(toRemove, std::greater<>());
        for (size_t idx : toRemove) {
          rows.erase(rows.begin() + static_cast<ptrdiff_t>(idx));
        }
      }
    };

    collapseWorkspaceRuns(KeybindAction::WorkspaceSwitch);
    collapseWorkspaceRuns(KeybindAction::WindowMoveToWorkspace);

    // --- Step 2: Group rows ---
    // Fixed groups in display order, then submaps in first-seen order.
    constexpr Group kFixedGroups[] = {
        Group::Apps, Group::Focus, Group::MoveSize, Group::Windows, Group::Workspaces, Group::Overview, Group::System,
    };

    // Collect submaps in first-seen order.
    std::vector<std::string> submapOrder;
    for (const auto& row : rows) {
      if (!row.submap.empty()) {
        if (std::ranges::find(submapOrder, row.submap) == submapOrder.end()) {
          submapOrder.push_back(row.submap);
        }
      }
    }

    // A display line is either a group header or a bind row.
    struct DisplayLine {
      bool isHeader = false;
      bool isDitto = false;
      int group = 0;    // group index: lines with the same value are never split across columns
      std::string text; // Pango markup for the full line (header or row)
    };

    auto buildLines = [&]() -> std::vector<DisplayLine> {
      std::vector<DisplayLine> lines;
      int groupId = 0;

      for (Group grp : kFixedGroups) {
        std::vector<const CheatsheetRow*> groupRows;
        for (const auto& row : rows) {
          if (!row.submap.empty())
            continue;
          if (groupForAction(row.actionType) == grp) {
            groupRows.push_back(&row);
          }
        }
        if (groupRows.empty())
          continue;

        const char* title = groupTitle(grp);
        // Blank separator belongs to the upcoming group (break happens before it).
        ++groupId;
        if (!lines.empty()) {
          lines.push_back({.isHeader = false, .group = groupId, .text = ""});
        }
        lines.push_back({
            .isHeader = true,
            .group = groupId,
            .text = std::format("<span foreground='#f5c96b' weight='bold'>{}</span>", title),
        });

        // Find max chord width in this group (character count, for padding).
        size_t maxChordLen = 0;
        for (const auto* row : groupRows) {
          maxChordLen = std::max(maxChordLen, row->chord.size());
        }

        // Apps group: single-usage binaries stay here as flat rows.
        // Multi-usage binaries are deferred to their own top-level groups
        // (rendered right after Apps with the same header style).
        if (grp == Group::Apps) {
          // Collect unique binaries in first-seen order.
          std::vector<std::string> binOrder;
          for (const auto* row : groupRows) {
            const std::string& bin = row->spawnBinary;
            if (std::ranges::find(binOrder, bin) == binOrder.end()) {
              binOrder.push_back(bin);
            }
          }

          // Partition: single-usage binaries render flat under "Apps",
          // multi-usage binaries are collected for their own groups.
          struct DeferredGroup {
            std::string title;
            std::vector<const CheatsheetRow*> rows;
          };
          std::vector<DeferredGroup> deferred;

          for (const auto& bin : binOrder) {
            std::vector<const CheatsheetRow*> binRows;
            for (const auto* row : groupRows) {
              if (row->spawnBinary == bin) {
                binRows.push_back(row);
              }
            }

            const auto realCount =
                std::ranges::count_if(binRows, [](const CheatsheetRow* r) { return r->action != "\xe2\x80\xb3"; });
            const bool promote = realCount >= 2
                && std::ranges::any_of(binRows, [](const CheatsheetRow* r) { return !r->spawnArgs.empty(); });

            if (promote) {
              // Capitalize first letter for the group title.
              std::string groupTitle = bin;
              if (!groupTitle.empty()) {
                groupTitle[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(groupTitle[0])));
              }
              deferred.push_back({.title = std::move(groupTitle), .rows = std::move(binRows)});
            } else {
              // Flat under "Apps": combine binary + args.
              for (const auto* row : binRows) {
                std::string escapedChord = escape(row->chord);
                const bool isDitto = row->action == "\xe2\x80\xb3";
                std::string label;
                if (isDitto) {
                  label = row->action;
                } else {
                  label = row->spawnArgs.empty() ? row->spawnBinary : row->spawnBinary + " " + row->action;
                  if (label.size() > 32) {
                    label = label.substr(0, 32) + "\xe2\x80\xa6";
                  }
                }
                std::string escapedAction = escape(label);
                size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
                std::string padding(extraPad, ' ');
                const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
                lines.push_back({
                    .isHeader = false,
                    .isDitto = isDitto,
                    .group = groupId,
                    .text = std::format(
                        "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span "
                        "foreground='{}'>{}</span>",
                        escapedChord, padding, actionColor, escapedAction
                    ),
                });
              }
            }
          }

          // Render deferred binary groups as top-level sections.
          for (auto& dg : deferred) {
            ++groupId;
            if (!lines.empty()) {
              lines.push_back({.isHeader = false, .group = groupId, .text = ""});
            }
            lines.push_back({
                .isHeader = true,
                .group = groupId,
                .text = std::format("<span foreground='#f5c96b' weight='bold'>{}</span>", escape(dg.title)),
            });
            for (const auto* row : dg.rows) {
              std::string escapedChord = escape(row->chord);
              const bool isDitto = row->action == "\xe2\x80\xb3";
              std::string escapedAction = escape(isDitto ? row->action : row->action);
              size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
              std::string padding(extraPad, ' ');
              const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
              lines.push_back({
                  .isHeader = false,
                  .isDitto = isDitto,
                  .group = groupId,
                  .text = std::format(
                      "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span foreground='{}'>{}</span>",
                      escapedChord, padding, actionColor, escapedAction
                  ),
              });
            }
          }
          continue;
        }

        // All other groups: flat row rendering.
        for (const auto* row : groupRows) {
          std::string escapedChord = escape(row->chord);
          std::string escapedAction = escape(row->action);
          size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
          std::string padding(extraPad, ' ');
          const bool isDitto = row->action == "\xe2\x80\xb3";
          const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
          lines.push_back({
              .isHeader = false,
              .isDitto = isDitto,
              .group = groupId,
              .text = std::format(
                  "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span foreground='{}'>{}</span>",
                  escapedChord, padding, actionColor, escapedAction
              ),
          });
        }
      }

      // Submap groups.
      for (const auto& smName : submapOrder) {
        std::vector<const CheatsheetRow*> groupRows;
        for (const auto& row : rows) {
          if (row.submap == smName) {
            groupRows.push_back(&row);
          }
        }
        if (groupRows.empty())
          continue;

        ++groupId;
        if (!lines.empty()) {
          lines.push_back({.isHeader = false, .group = groupId, .text = ""});
        }
        lines.push_back({
            .isHeader = true,
            .group = groupId,
            .text = std::format("<span foreground='#f5c96b' weight='bold'>Submap: {}</span>", escape(smName)),
        });

        size_t maxChordLen = 0;
        for (const auto* row : groupRows) {
          maxChordLen = std::max(maxChordLen, row->chord.size());
        }

        for (const auto* row : groupRows) {
          std::string escapedChord = escape(row->chord);
          std::string escapedAction = escape(row->action);
          size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
          std::string padding(extraPad, ' ');
          const bool isDitto = row->action == "\xe2\x80\xb3";
          const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
          lines.push_back({
              .isHeader = false,
              .isDitto = isDitto,
              .group = groupId,
              .text = std::format(
                  "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span foreground='{}'>{}</span>",
                  escapedChord, padding, actionColor, escapedAction
              ),
          });
        }
      }

      return lines;
    };

    auto allLines = buildLines();

    // --- Handle empty bind list ---
    if (allLines.empty()) {
      allLines.push_back({
          .isHeader = false,
          .text = "<span foreground='#8a8a92'>no keybinds configured</span>",
      });
    }

    // --- Step 4: Column layout ---
    auto layoutColumns = [&](int numCols) -> std::vector<std::string> {
      const int lineCount = static_cast<int>(allLines.size());
      const int linesPerCol = (lineCount + numCols - 1) / numCols;
      std::vector<std::string> columns;

      // Collect group boundary indices (first line of each group).
      std::vector<int> groupStarts;
      if (lineCount > 0) {
        groupStarts.push_back(0);
        for (int i = 1; i < lineCount; ++i) {
          if (allLines[static_cast<size_t>(i)].group != allLines[static_cast<size_t>(i - 1)].group) {
            groupStarts.push_back(i);
          }
        }
      }

      int pos = 0;
      for (int col = 0; col < numCols && pos < lineCount; ++col) {
        std::string markup;
        int colEnd = std::min(pos + linesPerCol, lineCount);

        // Snap colEnd back to the nearest group boundary so no group is split.
        if (colEnd < lineCount) {
          // Find the largest group start <= colEnd.
          auto it = std::ranges::upper_bound(groupStarts, colEnd);
          if (it != groupStarts.begin()) {
            --it;
            // Only pull back if it doesn't collapse the column to nothing.
            if (*it > pos) {
              colEnd = *it;
            }
          }
        }

        for (int i = pos; i < colEnd; ++i) {
          if (i > pos) {
            markup += '\n';
          }
          markup += allLines[static_cast<size_t>(i)].text;
        }
        if (!markup.empty()) {
          columns.push_back(std::move(markup));
        }
        pos = colEnd;
      }

      // Remaining lines go into the last column.
      if (pos < lineCount && !columns.empty()) {
        auto& last = columns.back();
        for (int i = pos; i < lineCount; ++i) {
          last += '\n';
          last += allLines[static_cast<size_t>(i)].text;
        }
      }

      return columns;
    };

    int lineCount = static_cast<int>(allLines.size());
    int numCols = std::clamp((lineCount + 17) / 18, 1, 3);

    // --- Step 5: Header/footer buffers ---
    std::string titleMarkup = "<span size='14pt' weight='bold' foreground='#7aa3ff'>Umbriel keybinds</span>";
    if (configFileMissing()) {
      titleMarkup += "\n<span foreground='#f5c96b'>no config found \xc2\xb7 showing built-in defaults</span>";
      titleMarkup += "\n<span foreground='#8a8a92'>copy example.toml to ~/.config/umbriel/config.toml</span>";
    }

    const char* modName = m_server.nested() ? "Alt" : "Super";
    std::string footerMarkup =
        std::format("<span foreground='#8a8a92'>Mod = {} \xc2\xb7 press any key to close</span>", modName);

    // Render title and footer.
    TextBufferResult titleBuf = renderTextBuffer({
        .markup = titleMarkup,
        .font = "monospace 11",
        .maxWidth = 900,
        .padding = 0,
        .scale = scale,
        .bgR = 0.0,
        .bgG = 0.0,
        .bgB = 0.0,
        .bgA = 0.0,
    });
    TextBufferResult footerBuf = renderTextBuffer({
        .markup = footerMarkup,
        .font = "monospace 11",
        .maxWidth = 900,
        .padding = 0,
        .scale = scale,
        .bgR = 0.0,
        .bgG = 0.0,
        .bgB = 0.0,
        .bgA = 0.0,
    });

    // Render columns, potentially retrying with more columns if too tall.
    auto renderColumns = [&](int cols) -> std::pair<std::vector<TextBufferResult>, int> {
      auto colMarkups = layoutColumns(cols);
      std::vector<TextBufferResult> buffers;
      int maxH = 0;
      for (auto& markup : colMarkups) {
        TextBufferResult buf = renderTextBuffer({
            .markup = std::move(markup),
            .font = "monospace 11",
            .maxWidth = kColumnMaxWidth,
            .padding = 0,
            .scale = scale,
            .bgR = 0.0,
            .bgG = 0.0,
            .bgB = 0.0,
            .bgA = 0.0,
        });
        maxH = std::max(maxH, buf.logicalHeight);
        buffers.push_back(buf);
      }
      return {std::move(buffers), maxH};
    };

    auto [colBufs, maxColH] = renderColumns(numCols);

    // Check if panel exceeds output height.
    int totalH = titleBuf.logicalHeight + kTitleBodyGap + maxColH + kBodyFooterGap + footerBuf.logicalHeight + 2 * kPad;
    if (haveOutput && totalH > outputBox.height - 120 && numCols < kMaxColumns) {
      // Drop previous column buffers.
      for (auto& buf : colBufs) {
        if (buf.buffer != nullptr)
          wlr_buffer_drop(buf.buffer);
      }
      numCols = std::min(numCols + 1, kMaxColumns);
      auto [newBufs, newMaxH] = renderColumns(numCols);
      colBufs = std::move(newBufs);
      maxColH = newMaxH;
      totalH = titleBuf.logicalHeight + kTitleBodyGap + maxColH + kBodyFooterGap + footerBuf.logicalHeight + 2 * kPad;
    }

    // --- Step 6: Panel assembly ---
    int totalColW = 0;
    for (size_t i = 0; i < colBufs.size(); ++i) {
      totalColW += colBufs[i].logicalWidth;
      if (i + 1 < colBufs.size()) {
        totalColW += kColumnGap;
      }
    }

    int panelW = std::max({titleBuf.logicalWidth, footerBuf.logicalWidth, totalColW}) + 2 * kPad;
    int panelH = totalH;

    // Shadow.
    m_shadow.update(m_tree, panelW, panelH, 0, kCornerRadius, nullptr);

    // Panel rect.
    wlr_scene_rect* panelRect = wlr_scene_rect_create(m_tree, panelW, panelH, kPanelColor);
    wlr_scene_rect_set_corner_radius(panelRect, kCornerRadius);
    (void)panelRect;

    // Helper to add a text buffer to the scene tree.
    auto addBuffer = [this](TextBufferResult& result, int x, int y) {
      if (result.buffer == nullptr)
        return;
      wlr_scene_buffer* sceneBuf = wlr_scene_buffer_create(m_tree, result.buffer);
      wlr_buffer_drop(result.buffer);
      result.buffer = nullptr;
      if (sceneBuf == nullptr)
        return;
      wlr_scene_buffer_set_dest_size(sceneBuf, result.logicalWidth, result.logicalHeight);
      sceneBuf->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
      wlr_scene_node_set_position(&sceneBuf->node, x, y);
    };

    // Title.
    int curY = kPad;
    addBuffer(titleBuf, kPad, curY);
    curY += titleBuf.logicalHeight + kTitleBodyGap;

    // Columns.
    int colX = kPad;
    for (auto& buf : colBufs) {
      addBuffer(buf, colX, curY);
      colX += buf.logicalWidth + kColumnGap;
    }
    curY += maxColH + kBodyFooterGap;

    // Footer.
    addBuffer(footerBuf, kPad, curY);

    // Position: centered on preferred output.
    if (haveOutput) {
      int x = outputBox.x + (outputBox.width - panelW) / 2;
      int y = outputBox.y + (outputBox.height - panelH) / 2;
      wlr_scene_node_set_position(&m_tree->node, x, y);
    } else {
      wlr_scene_node_set_position(&m_tree->node, 24, 24);
    }
  }

} // namespace umbriel
