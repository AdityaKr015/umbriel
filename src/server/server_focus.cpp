#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/node.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cstdlib>
#include <ranges>

namespace umbriel {

  void Server::focusView(View* view, FocusReason reason) {
    if (view == nullptr || m_sessionLocked) {
      return;
    }

    // PointerHover gate: reject focus entirely when revealing would exceed the
    // configured max scroll fraction. Must run before any side effects (MRU,
    // seat focus) so an over-limit hover focuses nothing — preserving the
    // current behavior where cursor.cpp skipped focusView altogether.
    if (reason == FocusReason::PointerHover && view->tiled()) {
      if (Workspace* workspace = view->workspace()) {
        const auto& maxScroll = config().input.focus.followsMouseMaxScroll;
        if (maxScroll && workspace->scrollFractionToReveal(view) > *maxScroll) {
          return;
        }
      }
    }

    if (Workspace* workspace = view->workspace()) {
      if (!workspace->active()) {
        workspace->group()->activate(workspace);
      }
    }
    if (!view->onActiveWorkspace()) {
      return;
    }

    auto it = std::ranges::find_if(m_views, [view](const std::unique_ptr<View>& entry) { return entry.get() == view; });
    if (it != m_views.end() && it != m_views.begin()) {
      auto entry = std::move(*it);
      m_views.erase(it);
      m_views.insert(m_views.begin(), std::move(entry));
    }

    // Keep workspace focus while exclusive layer-shell holds the seat; refocus applies it later.
    // Still clear activation chrome so the previous window does not stay visually focused.
    // Overview owns the seat the same way, but keeps the chrome so card borders
    // track the focused window; the keyboard enter replays when it closes.
    const bool overviewActive = m_overview != nullptr && m_overview->active();
    const bool seatAvailable = exclusiveKeyboardLayer() == nullptr;
    if (seatAvailable) {
      view->applySeatFocus(!overviewActive);
    } else {
      deactivateViews(nullptr);
    }
    Workspace* workspace = view->workspace();
    if (workspace != nullptr) {
      workspace->setFocusedView(view);
    }
    if (overviewActive) {
      m_overview->onFocusChanged();
    }

    // Derive reveal policy from the focus reason.
    if (workspace == nullptr || !view->tiled()) {
      return;
    }
    switch (reason) {
    case FocusReason::Directional:
    case FocusReason::PointerPress:
    case FocusReason::PointerHover:
    case FocusReason::DragDrop:
    case FocusReason::Startup:
      workspace->ensureFocusedVisible();
      workspace->arrange(true);
      break;
    case FocusReason::Grab:
      // No reveal: the grab is about to move/detach the tile; revealing would
      // shift computed grab offsets and cause a visual jump.
      break;
    }
  }

  LayerSurface* Server::exclusiveKeyboardLayer() const {
    for (const auto& entry : m_layerSurfaces) {
      if (entry->exclusiveKeyboard()) {
        return entry.get();
      }
    }
    return nullptr;
  }

  void Server::refocus(Output* preferred) {
    if (m_sessionLocked) {
      return;
    }
    if (LayerSurface* layer = exclusiveKeyboardLayer()) {
      layer->focus();
      return;
    }

    const auto focusMappedOn = [this](Output* output) -> bool {
      if (output == nullptr || output->workspaceGroup() == nullptr) {
        return false;
      }
      Workspace* workspace = output->workspaceGroup()->active();
      if (workspace == nullptr) {
        return false;
      }
      if (View* focused = workspace->focusedView()) {
        if (focused->mapped() && focused->onActiveWorkspace()) {
          focusView(focused);
          return true;
        }
      }
      for (const auto& entry : m_views) {
        if (entry->mapped() && entry->workspace() == workspace) {
          focusView(entry.get());
          return true;
        }
      }
      return false;
    };

    if (preferred != nullptr) {
      if (focusMappedOn(preferred)) {
        return;
      }
      // Empty workspace on this output: clear focus instead of highlighting another display.
      clearKeyboardFocus();
      return;
    }

    Output* underCursor = outputFromWlr(preferredOutput());
    if (focusMappedOn(underCursor)) {
      return;
    }
    // Stay on the pointer's output: never steal focus onto another display when
    // this one has no mapped window (closing the last window on DP-1, empty WS, …).
    clearKeyboardFocus();
  }

  void Server::deactivateViews(View* except) {
    for (const auto& entry : m_views) {
      if (entry.get() == except || !entry->mapped()) {
        continue;
      }
      wlr_xdg_toplevel_set_activated(entry->toplevel(), false);
      entry->setBorderFocused(false);
      entry->setForeignActivated(false);
    }
  }

  void Server::clearKeyboardFocus() {
    deactivateViews(nullptr);
    wlr_seat_keyboard_notify_clear_focus(m_seat->wlr());
  }

  View* Server::viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer) {
    if (layer != nullptr) {
      *layer = nullptr;
    }

    wlr_scene_node* node = wlr_scene_node_at(&m_scene->tree.node, lx, ly, sx, sy);
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
      return nullptr;
    }

    wlr_scene_buffer* sceneBuffer = wlr_scene_buffer_from_node(node);
    wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(sceneBuffer);
    if (sceneSurface == nullptr) {
      return nullptr;
    }

    *surface = sceneSurface->surface;
    wlr_scene_tree* tree = node->parent;
    while (tree != nullptr && tree->node.data == nullptr) {
      tree = tree->node.parent;
    }
    if (tree == nullptr) {
      return nullptr;
    }

    auto* sceneNode = static_cast<SceneNode*>(tree->node.data);
    if (sceneNode->kind == SceneNodeKind::LockSurface) {
      return nullptr;
    }
    if (m_sessionLocked) {
      *surface = nullptr;
      return nullptr;
    }
    if (sceneNode->kind == SceneNodeKind::LayerSurface) {
      if (layer != nullptr) {
        *layer = static_cast<LayerSurface*>(sceneNode);
      }
      return nullptr;
    }
    return static_cast<View*>(sceneNode);
  }

  bool Server::handleVtSwitch(uint32_t keysym, uint32_t modifiers) {
    if (m_session == nullptr) {
      return false;
    }

    unsigned vt = 0;
    if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
      vt = 1 + (keysym - XKB_KEY_XF86Switch_VT_1);
    } else if (
        (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)
        && keysym >= XKB_KEY_F1
        && keysym <= XKB_KEY_F12
    ) {
      vt = 1 + (keysym - XKB_KEY_F1);
    }

    if (vt == 0) {
      return false;
    }

    if (!wlr_session_change_vt(m_session, vt)) {
      wlr_log(WLR_ERROR, "failed to switch to VT %u", vt);
    }
    return true;
  }

  bool Server::executeKeybindAction(const Keybind& bind, std::string* error) {
    if (error != nullptr) {
      error->clear();
    }
    auto activeWorkspace = [this]() -> Workspace* {
      Output* output = outputFromWlr(preferredOutput());
      if (output == nullptr || output->workspaceGroup() == nullptr) {
        return nullptr;
      }
      return output->workspaceGroup()->active();
    };

    switch (bind.action) {
    case KeybindAction::None:
      return false;
    case KeybindAction::Spawn:
      spawn(bind.spawnCommand.c_str());
      return true;
    case KeybindAction::WindowClose:
      if (Workspace* workspace = activeWorkspace()) {
        if (View* view = workspace->focusedView()) {
          wlr_xdg_toplevel_send_close(view->toplevel());
        }
      }
      return true;
    case KeybindAction::SessionQuit:
      stop();
      return true;
    case KeybindAction::ConfigReload:
      handleConfigReload();
      return true;
    case KeybindAction::WindowFocusLeft:
      if (Workspace* workspace = activeWorkspace()) {
        if (View* target = workspace->focusAdjacent(-1)) {
          focusView(target, FocusReason::Directional);
        }
      }
      return true;
    case KeybindAction::WindowFocusRight:
      if (Workspace* workspace = activeWorkspace()) {
        if (View* target = workspace->focusAdjacent(1)) {
          focusView(target, FocusReason::Directional);
        }
      }
      return true;
    case KeybindAction::WindowFocusUp:
      if (Workspace* workspace = activeWorkspace()) {
        if (View* target = workspace->focusVertical(-1)) {
          focusView(target, FocusReason::Directional);
        }
      }
      return true;
    case KeybindAction::WindowFocusDown:
      if (Workspace* workspace = activeWorkspace()) {
        if (View* target = workspace->focusVertical(1)) {
          focusView(target, FocusReason::Directional);
        }
      }
      return true;
    case KeybindAction::ColumnMoveLeft:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->moveFocusedColumn(-1);
      }
      return true;
    case KeybindAction::ColumnMoveRight:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->moveFocusedColumn(1);
      }
      return true;
    case KeybindAction::WindowMoveUp:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->moveFocusedVertical(-1);
      }
      return true;
    case KeybindAction::WindowMoveDown:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->moveFocusedVertical(1);
      }
      return true;
    case KeybindAction::WindowConsumeLeft:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->consumeFocusedLeft();
      }
      return true;
    case KeybindAction::WindowExpelRight:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->expelFocusedRight();
      }
      return true;
    case KeybindAction::WindowCycleWidth:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->cycleFocusedWidth();
      }
      return true;
    case KeybindAction::WindowSetWidth:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->setFocusedWidth(bind.widthFraction);
      }
      return true;
    case KeybindAction::ToggleMaximize:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->toggleFocusedFullWidth();
      }
      return true;
    case KeybindAction::ToggleFullscreen:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->toggleFocusedFullscreen();
      }
      return true;
    case KeybindAction::ToggleFloating:
      if (Workspace* workspace = activeWorkspace()) {
        workspace->toggleFocusedFloating();
      }
      return true;
    case KeybindAction::WindowFocusNext:
      if (m_views.size() >= 2) {
        for (size_t n = 0; n < m_views.size(); ++n) {
          auto current = std::move(m_views.front());
          m_views.erase(m_views.begin());
          m_views.push_back(std::move(current));
          if (m_views.front()->mapped() && m_views.front()->onActiveWorkspace()) {
            focusView(m_views.front().get(), FocusReason::Directional);
            break;
          }
        }
      }
      return true;
    case KeybindAction::WorkspaceSwitch:
    case KeybindAction::WindowMoveToWorkspace: {
      const auto reject = [error](std::string message) {
        if (error != nullptr) {
          *error = std::move(message);
        }
        return true;
      };

      Workspace* target = nullptr;
      if (!bind.workspaceOutput.empty()) {
        Output* output = outputFromName(bind.workspaceOutput);
        if (output == nullptr) {
          return reject("unknown output: " + bind.workspaceOutput);
        }
        WorkspaceGroup* group = output->workspaceGroup();
        if (group == nullptr) {
          return reject("output has no workspace group: " + bind.workspaceOutput);
        }
        target = group->workspaceNamed(bind.workspaceName);
        if (target == nullptr) {
          return reject("unknown workspace on output " + bind.workspaceOutput + ": " + bind.workspaceName);
        }
      } else {
        bool ambiguous = false;
        for (const auto& output : m_outputs) {
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
          return reject("unknown workspace: " + bind.workspaceName);
        }
        if (ambiguous) {
          Output* preferred = outputFromWlr(preferredOutput());
          WorkspaceGroup* preferredGroup = preferred != nullptr ? preferred->workspaceGroup() : nullptr;
          Workspace* preferredMatch =
              preferredGroup != nullptr ? preferredGroup->workspaceNamed(bind.workspaceName) : nullptr;
          if (preferredMatch == nullptr) {
            return reject(
                "ambiguous workspace: " + bind.workspaceName + " (qualify it as " + bind.workspaceName + "/<output>)"
            );
          }
          target = preferredMatch;
        }
      }

      WorkspaceGroup* group = target->group();
      if (bind.action == KeybindAction::WindowMoveToWorkspace) {
        for (const auto& entry : m_views) {
          if (entry->mapped() && entry->onActiveWorkspace()) {
            entry->setWorkspace(target);
            group->activate(target);
            focusView(entry.get(), FocusReason::Directional);
            return true;
          }
        }
      }
      group->select(target);
      return true;
    }
    case KeybindAction::LayoutScrollLeft:
    case KeybindAction::LayoutScrollRight:
      if (Workspace* workspace = activeWorkspace()) {
        if (workspace->layoutMode() == LayoutMode::Scrolling && workspace->group()->output() != nullptr) {
          const auto step = static_cast<double>(config().input.mouse.scrollWheelStep);
          const double delta = bind.action == KeybindAction::LayoutScrollLeft ? -step : step;
          // Clamp to the real scroll range: overscroll here would park the strip
          // past an edge and seed sub-pixel scroll residue.
          const int viewportWidth =
              std::max(1, workspace->group()->output()->usableArea().width - 2 * workspace->layoutConfig().edgePad);
          const auto maxScroll = static_cast<double>(workspace->layout().maxScroll(viewportWidth));
          workspace->layout().setScroll(std::clamp(workspace->layout().scroll() + delta, 0.0, maxScroll));
          workspace->arrange();
        }
      }
      return true;
    case KeybindAction::OverviewToggle:
      m_overview->toggle();
      return true;
    case KeybindAction::OverviewOpen:
      m_overview->open();
      return true;
    case KeybindAction::OverviewClose:
      m_overview->close();
      return true;
    case KeybindAction::CheatsheetToggle:
      if (m_cheatsheet != nullptr) {
        m_cheatsheet->toggle();
      }
      return true;
    case KeybindAction::CheatsheetOpen:
      if (m_cheatsheet != nullptr) {
        m_cheatsheet->show();
      }
      return true;
    case KeybindAction::CheatsheetClose:
      if (m_cheatsheet != nullptr) {
        m_cheatsheet->hide();
      }
      return true;
    case KeybindAction::Submap:
      if (bind.spawnCommand == "reset" || bind.spawnCommand == "disable") {
        if (m_activeSubmaps.empty()) {
          return false;
        }
        popSubmap();
      } else {
        pushSubmap(bind.spawnCommand);
      }
      return true;
    }
    return false;
  }

  const Keybind* Server::handleKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) {
    if (m_sessionLocked) {
      return nullptr;
    }

    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const uint32_t lowered = xkb_keysym_to_lower(keysym);
    const std::string& currentSubmap = m_activeSubmaps.empty() ? std::string{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        // Allow submap:reset / submap:disable from the default context to
        // always match, so users can define a global emergency exit.
        if (!m_activeSubmaps.empty()
            && bind.submap.empty()
            && bind.action == KeybindAction::Submap
            && (bind.spawnCommand == "reset" || bind.spawnCommand == "disable")) {
          // Fall through to match below.
        } else {
          continue;
        }
      }
      if (bind.wheel != WheelDirection::None || bind.mouseButton != 0) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected || (lowered != bind.keysym && rawKeysym != bind.keysym)) {
        continue;
      }
      return executeKeybindAction(bind) ? &bind : nullptr;
    }

    return nullptr;
  }

  bool Server::handleWheelBind(WheelDirection direction, uint32_t modifiers) {
    if (m_sessionLocked) {
      return false;
    }

    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string& currentSubmap = m_activeSubmaps.empty() ? std::string{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      if (bind.wheel != direction) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      return executeKeybindAction(bind);
    }

    return false;
  }

  bool Server::handleMouseBind(uint32_t button, uint32_t modifiers) {
    if (m_sessionLocked) {
      return false;
    }

    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string& currentSubmap = m_activeSubmaps.empty() ? std::string{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      // Non-mouse binds carry 0 here, which never equals a BTN_* code.
      if (bind.mouseButton != button) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      return executeKeybindAction(bind);
    }

    return false;
  }

  void Server::pushSubmap(const std::string& name) { m_activeSubmaps.push_back(name); }

  void Server::popSubmap() {
    if (!m_activeSubmaps.empty()) {
      m_activeSubmaps.pop_back();
    }
  }

  void Server::arrangeLayers(wlr_output* output) {
    if (Output* out = outputFromWlr(output)) {
      out->arrangeLayers();
    }
  }

  wlr_output* Server::preferredOutput() const {
    wlr_output* output = wlr_output_layout_output_at(m_outputLayout, m_cursor->wlr()->x, m_cursor->wlr()->y);
    if (output != nullptr) {
      return output;
    }
    if (!m_outputs.empty()) {
      return m_outputs.front()->wlr();
    }
    return nullptr;
  }

  Output* Server::outputFromWlr(wlr_output* output) const {
    if (output == nullptr) {
      return nullptr;
    }
    if (output->data != nullptr) {
      return static_cast<Output*>(output->data);
    }
    for (const auto& entry : m_outputs) {
      if (entry->wlr() == output) {
        return entry.get();
      }
    }
    return nullptr;
  }

  Output* Server::outputFromName(const std::string& name) const {
    for (const auto& entry : m_outputs) {
      if (entry->wlr()->name != nullptr && name == entry->wlr()->name) {
        return entry.get();
      }
    }
    return nullptr;
  }

  wlr_box Server::usableAreaAt(double lx, double ly) const {
    wlr_output* output = wlr_output_layout_output_at(m_outputLayout, lx, ly);
    if (output == nullptr) {
      output = preferredOutput();
    }
    if (Output* out = outputFromWlr(output)) {
      wlr_box usable = out->usableArea();
      if (usable.width > 0 && usable.height > 0) {
        return usable;
      }
    }

    wlr_box fullArea{};
    wlr_output_layout_get_box(m_outputLayout, output, &fullArea);
    return fullArea;
  }

} // namespace umbriel
