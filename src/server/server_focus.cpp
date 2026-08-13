#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/node.h"
#include "server/actions.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
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
      if (!view->pinned() && !workspace->active()) {
        workspace->group()->activate(workspace);
      }
    }
    if (!view->onActiveWorkspace() && !view->pinned()) {
      return;
    }
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->noteFocus(view);
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
    if (workspace != nullptr && (!view->pinned() || workspace->active())) {
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
    SceneNode* sceneNode = nullptr;
    while (tree != nullptr && (sceneNode = sceneNodeFrom(tree->node.data)) == nullptr) {
      tree = tree->node.parent;
    }
    if (sceneNode == nullptr) {
      return nullptr;
    }

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
    if (bind.action == KeybindAction::None) {
      return false;
    }
    const ActionHandlerFn handler = actionHandlerFor(bind.action);
    if (handler == nullptr) {
      if (error != nullptr) {
        *error = "action has no handler";
      }
      return false;
    }
    return handler(*this, bind, error);
  }

  bool Server::focusNextWindow() {
    if (m_views.size() < 2) {
      return false;
    }
    for (size_t n = 0; n < m_views.size(); ++n) {
      auto current = std::move(m_views.front());
      m_views.erase(m_views.begin());
      m_views.push_back(std::move(current));
      if (m_views.front()->mapped() && m_views.front()->onActiveWorkspace()) {
        focusView(m_views.front().get(), FocusReason::Directional);
        return true;
      }
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
        if (!m_activeSubmaps.empty() && bind.submap.empty() && isSubmapResetBind(bind)) {
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
