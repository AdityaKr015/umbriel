#include "server/actions.h"

#include "config/config.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <expected>
#include <utility>

namespace umbriel {

  namespace {

    Workspace* activeWorkspace(Server& server) {
      Output* output = server.outputFromWlr(server.preferredOutput());
      if (output == nullptr || output->workspaceGroup() == nullptr) {
        return nullptr;
      }
      return output->workspaceGroup()->active();
    }

    Output* scratchpadOutput(Server& server, const Keybind& bind, std::string* error) {
      if (bind.scratchpadOutput.empty()) {
        return server.outputFromWlr(server.preferredOutput());
      }
      Output* output = server.outputFromName(bind.scratchpadOutput);
      if (output == nullptr && error != nullptr) {
        *error = "unknown output: " + bind.scratchpadOutput;
      }
      return output;
    }

    // The action was consumed, but with a message for the caller.
    bool reject(std::string* error, std::string message) {
      if (error != nullptr) {
        *error = std::move(message);
      }
      return true;
    }

    // Resolve `<workspace>` or `<workspace>/<output>` against the current output
    // layout. Qualified selectors address exactly one group; unqualified ones
    // prefer the focused output, then a unique match anywhere.
    std::expected<Workspace*, std::string> resolveWorkspaceSelector(Server& server, const Keybind& bind) {
      if (!bind.workspaceOutput.empty()) {
        Output* output = server.outputFromName(bind.workspaceOutput);
        if (output == nullptr) {
          return std::unexpected("unknown output: " + bind.workspaceOutput);
        }
        WorkspaceGroup* group = output->workspaceGroup();
        if (group == nullptr) {
          return std::unexpected("output has no workspace group: " + bind.workspaceOutput);
        }
        Workspace* target = group->workspaceForSelector(bind.workspaceName);
        if (target == nullptr) {
          return std::unexpected("unknown workspace on output " + bind.workspaceOutput + ": " + bind.workspaceName);
        }
        return target;
      }

      Output* preferred = server.outputFromWlr(server.preferredOutput());
      WorkspaceGroup* preferredGroup = preferred != nullptr ? preferred->workspaceGroup() : nullptr;

      // Dynamic numbered workspaces belong to their output. Resolve this first
      // so an existing number elsewhere cannot steal a local request.
      const bool numericSelector = !bind.workspaceName.empty()
          && std::ranges::all_of(bind.workspaceName, [](char value) { return value >= '0' && value <= '9'; });
      if (preferredGroup != nullptr && preferredGroup->dynamic() && numericSelector) {
        if (Workspace* target = preferredGroup->workspaceForSelector(bind.workspaceName)) {
          return target;
        }
      }

      Workspace* target = nullptr;
      bool ambiguous = false;
      for (const auto& output : server.outputs()) {
        WorkspaceGroup* group = output->workspaceGroup();
        Workspace* match = group != nullptr ? group->workspaceNamed(bind.workspaceName) : nullptr;
        if (match == nullptr) {
          continue;
        }
        if (target != nullptr) {
          ambiguous = true;
        } else {
          target = match;
        }
      }

      if (target == nullptr) {
        target = preferredGroup != nullptr ? preferredGroup->workspaceForSelector(bind.workspaceName) : nullptr;
        if (target == nullptr) {
          return std::unexpected("unknown workspace: " + bind.workspaceName);
        }
        return target;
      }

      if (ambiguous) {
        Workspace* preferredMatch =
            preferredGroup != nullptr ? preferredGroup->workspaceNamed(bind.workspaceName) : nullptr;
        if (preferredMatch == nullptr) {
          return std::unexpected(
              "ambiguous workspace: " + bind.workspaceName + " (qualify it as " + bind.workspaceName + "/<output>)"
          );
        }
        return preferredMatch;
      }
      return target;
    }

    // A scratchpad window owns the focus, so window-level toggles are inert.
    bool scratchpadHoldsFocus(Server& server) {
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->hasFocus(server.outputFromWlr(server.preferredOutput()));
    }

    // ---- Session ----

    bool actionSpawn(Server& server, const Keybind& bind, std::string* /*error*/) {
      server.spawn(bind.spawnCommand.c_str());
      return true;
    }

    bool actionSessionQuit(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.stop();
      return true;
    }

    bool actionConfigReload(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.handleConfigReload();
      return true;
    }

    bool actionSubmap(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (bind.spawnCommand == "reset" || bind.spawnCommand == "disable") {
        if (!server.inSubmap()) {
          return false;
        }
        server.popSubmap();
      } else {
        server.pushSubmap(bind.spawnCommand);
      }
      return true;
    }

    // ---- Window ----

    bool actionWindowClose(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (ScratchpadManager* scratchpad = server.scratchpadManager()) {
        if (Output* output = server.outputFromWlr(server.preferredOutput())) {
          if (View* view = scratchpad->focused(output)) {
            wlr_xdg_toplevel_send_close(view->toplevel());
            return true;
          }
        }
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* view = workspace->focusedView()) {
          wlr_xdg_toplevel_send_close(view->toplevel());
        }
      }
      return true;
    }

    template <int Direction> bool actionFocusAdjacent(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* target = workspace->focusAdjacent(Direction)) {
          server.focusView(target, FocusReason::Directional);
        }
      }
      return true;
    }

    template <int Direction> bool actionFocusVertical(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* target = workspace->focusVertical(Direction)) {
          server.focusView(target, FocusReason::Directional);
        }
      }
      return true;
    }

    template <int Direction> bool actionMoveColumn(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->moveFocusedColumn(Direction);
      }
      return true;
    }

    template <int Direction> bool actionMoveVertical(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->moveFocusedVertical(Direction);
      }
      return true;
    }

    bool actionConsumeLeft(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->consumeFocusedLeft();
      }
      return true;
    }

    bool actionExpelRight(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->expelFocusedRight();
      }
      return true;
    }

    bool actionCycleWidth(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->cycleFocusedWidth();
      }
      return true;
    }

    bool actionSetWidth(Server& server, const Keybind& bind, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->setFocusedWidth(bind.widthFraction);
      }
      return true;
    }

    bool actionToggleMaximize(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedFullWidth();
      }
      return true;
    }

    bool actionToggleFullscreen(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedFullscreen();
      }
      return true;
    }

    bool actionToggleFloating(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        workspace->toggleFocusedFloating();
      }
      return true;
    }

    bool actionTogglePinned(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (scratchpadHoldsFocus(server)) {
        return true;
      }
      if (Workspace* workspace = activeWorkspace(server)) {
        if (View* view = workspace->focusedView()) {
          view->togglePinned();
        }
      }
      return true;
    }

    bool actionFocusNext(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.focusNextWindow();
      return true;
    }

    // ---- Workspaces ----

    bool actionWorkspace(Server& server, const Keybind& bind, std::string* error) {
      const std::expected<Workspace*, std::string> target = resolveWorkspaceSelector(server, bind);
      if (!target.has_value()) {
        return reject(error, target.error());
      }

      WorkspaceGroup* group = (*target)->group();
      if (bind.action == KeybindAction::WindowMoveToWorkspace) {
        for (const auto& entry : server.views()) {
          if (entry->mapped() && entry->onActiveWorkspace()) {
            entry->setWorkspace(*target);
            group->activate(*target);
            server.focusView(entry.get(), FocusReason::Directional);
            return true;
          }
        }
      }
      group->select(*target);
      return true;
    }

    template <int Sign> bool actionLayoutScroll(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      Workspace* workspace = activeWorkspace(server);
      if (workspace == nullptr
          || workspace->layoutMode() != LayoutMode::Scrolling
          || workspace->group()->output() == nullptr) {
        return true;
      }
      const auto step = static_cast<double>(config().input.mouse.scrollWheelStep);
      const double delta = Sign * step;
      // Clamp to the real scroll range: overscroll here would park the strip
      // past an edge and seed sub-pixel scroll residue.
      const int viewportWidth =
          std::max(1, workspace->group()->output()->usableArea().width - 2 * workspace->layoutConfig().edgePad);
      const auto maxScroll = static_cast<double>(workspace->layout().maxScroll(viewportWidth));
      workspace->layout().setScroll(std::clamp(workspace->layout().scroll() + delta, 0.0, maxScroll));
      workspace->arrange();
      return true;
    }

    // ---- Overlays ----

    bool actionOverviewToggle(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.overview()->toggle();
      return true;
    }

    bool actionOverviewOpen(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.overview()->open();
      return true;
    }

    bool actionOverviewClose(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      server.overview()->close();
      return true;
    }

    bool actionCheatsheetToggle(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Cheatsheet* sheet = server.cheatsheet()) {
        sheet->toggle();
      }
      return true;
    }

    bool actionCheatsheetOpen(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Cheatsheet* sheet = server.cheatsheet()) {
        sheet->show();
      }
      return true;
    }

    bool actionCheatsheetClose(Server& server, const Keybind& /*bind*/, std::string* /*error*/) {
      if (Cheatsheet* sheet = server.cheatsheet()) {
        sheet->hide();
      }
      return true;
    }

    // ---- Scratchpad ----

    bool actionMoveToScratchpad(Server& server, const Keybind& bind, std::string* error) {
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      Workspace* workspace = activeWorkspace(server);
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr
          && workspace != nullptr
          && scratchpad->moveToScratchpad(workspace->focusedView(), output);
    }

    bool actionScratchpadToggle(Server& server, const Keybind& bind, std::string* error) {
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->toggle(output);
    }

    bool actionRestoreFromScratchpad(Server& server, const Keybind& bind, std::string* error) {
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->restoreFocused(output);
    }

    bool actionScratchpadFocusNext(Server& server, const Keybind& bind, std::string* error) {
      Output* output = scratchpadOutput(server, bind, error);
      if (output == nullptr) {
        return false;
      }
      ScratchpadManager* scratchpad = server.scratchpadManager();
      return scratchpad != nullptr && scratchpad->focusNext(output);
    }

    struct ActionEntry {
      KeybindAction action;
      ActionHandlerFn run;
    };

    constexpr ActionEntry kActionHandlers[] = {
        {KeybindAction::Spawn, &actionSpawn},
        {KeybindAction::WindowClose, &actionWindowClose},
        {KeybindAction::SessionQuit, &actionSessionQuit},
        {KeybindAction::WindowFocusLeft, &actionFocusAdjacent<-1>},
        {KeybindAction::WindowFocusRight, &actionFocusAdjacent<1>},
        {KeybindAction::WindowFocusUp, &actionFocusVertical<-1>},
        {KeybindAction::WindowFocusDown, &actionFocusVertical<1>},
        {KeybindAction::ColumnMoveLeft, &actionMoveColumn<-1>},
        {KeybindAction::ColumnMoveRight, &actionMoveColumn<1>},
        {KeybindAction::WindowMoveUp, &actionMoveVertical<-1>},
        {KeybindAction::WindowMoveDown, &actionMoveVertical<1>},
        {KeybindAction::WindowConsumeLeft, &actionConsumeLeft},
        {KeybindAction::WindowExpelRight, &actionExpelRight},
        {KeybindAction::WindowCycleWidth, &actionCycleWidth},
        {KeybindAction::WindowSetWidth, &actionSetWidth},
        {KeybindAction::ToggleMaximize, &actionToggleMaximize},
        {KeybindAction::ToggleFullscreen, &actionToggleFullscreen},
        {KeybindAction::ToggleFloating, &actionToggleFloating},
        {KeybindAction::TogglePinned, &actionTogglePinned},
        {KeybindAction::WindowFocusNext, &actionFocusNext},
        {KeybindAction::WorkspaceSwitch, &actionWorkspace},
        {KeybindAction::WindowMoveToWorkspace, &actionWorkspace},
        {KeybindAction::ConfigReload, &actionConfigReload},
        {KeybindAction::LayoutScrollLeft, &actionLayoutScroll<-1>},
        {KeybindAction::LayoutScrollRight, &actionLayoutScroll<1>},
        {KeybindAction::OverviewToggle, &actionOverviewToggle},
        {KeybindAction::OverviewOpen, &actionOverviewOpen},
        {KeybindAction::OverviewClose, &actionOverviewClose},
        {KeybindAction::CheatsheetToggle, &actionCheatsheetToggle},
        {KeybindAction::CheatsheetOpen, &actionCheatsheetOpen},
        {KeybindAction::CheatsheetClose, &actionCheatsheetClose},
        {KeybindAction::WindowMoveToScratchpad, &actionMoveToScratchpad},
        {KeybindAction::ScratchpadToggle, &actionScratchpadToggle},
        {KeybindAction::WindowRestoreFromScratchpad, &actionRestoreFromScratchpad},
        {KeybindAction::ScratchpadFocusNext, &actionScratchpadFocusNext},
        {KeybindAction::Submap, &actionSubmap},
    };

  } // namespace

  ActionHandlerFn actionHandlerFor(KeybindAction action) {
    for (const ActionEntry& entry : kActionHandlers) {
      if (entry.action == action) {
        return entry.run;
      }
    }
    return nullptr;
  }

} // namespace umbriel
