#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "output/output.h"
#include "scene/node.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <cstdlib>

namespace umbriel {

  void Server::focusView(View* view, bool bringIntoView, bool animate) {
    if (view == nullptr || m_sessionLocked) {
      return;
    }
    // Exclusive layer-shell keyboard grab must not be stolen by window focus.
    if (exclusiveKeyboardLayer() != nullptr) {
      return;
    }
    if (Workspace* workspace = view->workspace()) {
      if (!workspace->active()) {
        workspace->group()->activate(workspace);
      }
    }
    if (!view->onActiveWorkspace()) {
      return;
    }

    for (const auto& entry : m_views) {
      if (entry.get() != view) {
        entry->setForeignActivated(false);
      }
    }

    auto it = std::find_if(m_views.begin(), m_views.end(), [view](const std::unique_ptr<View>& entry) {
      return entry.get() == view;
    });
    if (it != m_views.end() && it != m_views.begin()) {
      auto entry = std::move(*it);
      m_views.erase(it);
      m_views.insert(m_views.begin(), std::move(entry));
    }

    view->focus();
    if (Workspace* workspace = view->workspace()) {
      workspace->setFocusedView(view);
      if (bringIntoView && view->tiled()) {
        workspace->ensureFocusedVisible();
        workspace->arrange(animate);
      }
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
    for (const auto& entry : m_views) {
      if (entry->mapped() && entry->onActiveWorkspace()) {
        focusView(entry.get());
        return;
      }
    }
    clearKeyboardFocus();
  }

  void Server::clearKeyboardFocus() {
    wlr_seat* seat = m_seat->wlr();
    if (wlr_surface* prev = seat->keyboard_state.focused_surface) {
      if (wlr_xdg_toplevel* prevToplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev)) {
        wlr_xdg_toplevel_set_activated(prevToplevel, false);
        auto* prevTree = static_cast<wlr_scene_tree*>(prevToplevel->base->data);
        if (prevTree != nullptr && prevTree->node.data != nullptr) {
          static_cast<View*>(prevTree->node.data)->setBorderFocused(false);
        }
      }
    }
    wlr_seat_keyboard_notify_clear_focus(seat);
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

  bool Server::handleKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) {
    if (m_sessionLocked) {
      return false;
    }

    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const uint32_t lowered = xkb_keysym_to_lower(keysym);

    auto activeWorkspace = [this]() -> Workspace* {
      Output* output = outputFromWlr(preferredOutput());
      if (output == nullptr || output->workspaceGroup() == nullptr) {
        return nullptr;
      }
      return output->workspaceGroup()->active();
    };

    for (const Keybind& bind : config().keybinds) {
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected || (lowered != bind.keysym && rawKeysym != bind.keysym)) {
        continue;
      }

      switch (bind.action) {
      case KeybindAction::None:
        return true;
      case KeybindAction::Spawn:
        spawn(bind.spawnCommand.c_str());
        return true;
      case KeybindAction::SpawnTerminal: {
        const std::string& configured = config().general.terminal;
        const char* terminal = !configured.empty() ? configured.c_str() : std::getenv("TERMINAL");
        if (terminal == nullptr || terminal[0] == '\0') {
          wlr_log(WLR_ERROR, "mod+Return: set [general].terminal or $TERMINAL");
          return true;
        }
        spawn(terminal);
        return true;
      }
      case KeybindAction::Close:
        if (Workspace* workspace = activeWorkspace()) {
          if (View* view = workspace->focusedView()) {
            wlr_xdg_toplevel_send_close(view->toplevel());
          }
        }
        return true;
      case KeybindAction::Quit:
        stop();
        return true;
      case KeybindAction::ReloadConfig:
        handleConfigReload();
        return true;
      case KeybindAction::FocusLeft:
        if (Workspace* workspace = activeWorkspace()) {
          if (View* target = workspace->focusAdjacent(-1)) {
            focusView(target);
          }
        }
        return true;
      case KeybindAction::FocusRight:
        if (Workspace* workspace = activeWorkspace()) {
          if (View* target = workspace->focusAdjacent(1)) {
            focusView(target);
          }
        }
        return true;
      case KeybindAction::FocusUp:
        if (Workspace* workspace = activeWorkspace()) {
          if (View* target = workspace->focusVertical(-1)) {
            focusView(target);
          }
        }
        return true;
      case KeybindAction::FocusDown:
        if (Workspace* workspace = activeWorkspace()) {
          if (View* target = workspace->focusVertical(1)) {
            focusView(target);
          }
        }
        return true;
      case KeybindAction::MoveColumnLeft:
        if (Workspace* workspace = activeWorkspace()) {
          workspace->moveFocusedColumn(-1);
        }
        return true;
      case KeybindAction::MoveColumnRight:
        if (Workspace* workspace = activeWorkspace()) {
          workspace->moveFocusedColumn(1);
        }
        return true;
      case KeybindAction::MoveUp:
        if (Workspace* workspace = activeWorkspace()) {
          workspace->moveFocusedVertical(-1);
        }
        return true;
      case KeybindAction::MoveDown:
        if (Workspace* workspace = activeWorkspace()) {
          workspace->moveFocusedVertical(1);
        }
        return true;
      case KeybindAction::ConsumeLeft:
        if (Workspace* workspace = activeWorkspace()) {
          workspace->consumeFocusedLeft();
        }
        return true;
      case KeybindAction::ExpelRight:
        if (Workspace* workspace = activeWorkspace()) {
          workspace->expelFocusedRight();
        }
        return true;
      case KeybindAction::CycleWidth:
        if (Workspace* workspace = activeWorkspace()) {
          workspace->cycleFocusedWidth();
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
      case KeybindAction::FocusNext:
        if (m_views.size() >= 2) {
          for (size_t n = 0; n < m_views.size(); ++n) {
            auto current = std::move(m_views.front());
            m_views.erase(m_views.begin());
            m_views.push_back(std::move(current));
            if (m_views.front()->mapped() && m_views.front()->onActiveWorkspace()) {
              focusView(m_views.front().get());
              break;
            }
          }
        }
        return true;
      case KeybindAction::Workspace:
      case KeybindAction::MoveToWorkspace: {
        Output* output = outputFromWlr(preferredOutput());
        if (output == nullptr || output->workspaceGroup() == nullptr) {
          return true;
        }
        WorkspaceGroup* group = output->workspaceGroup();
        const size_t index = static_cast<size_t>(bind.workspace);
        Workspace* target = group->workspaceAt(index);
        if (target == nullptr) {
          return true;
        }
        if (bind.action == KeybindAction::MoveToWorkspace) {
          for (const auto& entry : m_views) {
            if (entry->mapped() && entry->onActiveWorkspace()) {
              entry->setWorkspace(target);
              group->activate(target);
              focusView(entry.get());
              return true;
            }
          }
        }
        group->activateIndex(index);
        return true;
      }
      }
    }

    return false;
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

  void Server::prepareSceneForOutput(Output* rendering) {
    if (rendering == nullptr) {
      return;
    }
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_outputLayout, rendering->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return;
    }

    View* grabbed = nullptr;
    if (m_cursor != nullptr && (m_cursor->mode() == CursorMode::MoveTile || m_cursor->mode() == CursorMode::Move)) {
      grabbed = m_cursor->grabbedView();
    }

    for (const auto& entry : m_views) {
      View* view = entry.get();
      if (view == nullptr || !view->mapped() || view->sceneTree() == nullptr) {
        continue;
      }

      if (view == grabbed) {
        const wlr_box& geometry = view->toplevel()->base->geometry;
        const wlr_box target{
            .x = view->sceneTree()->node.x,
            .y = view->sceneTree()->node.y,
            .width = geometry.width,
            .height = geometry.height,
        };
        const int border = view->tiled() ? config().appearance.totalBorderWidth() : 0;
        wlr_box decorated = target;
        decorated.x -= border;
        decorated.y -= border;
        decorated.width += 2 * border;
        decorated.height += 2 * border;
        wlr_box intersection{};
        if (!wlr_box_intersection(&intersection, &decorated, &outputBox)) {
          wlr_scene_node_set_enabled(&view->sceneTree()->node, false);
          continue;
        }
        wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
        view->setOutputClip(&intersection, target, outputBox);
        continue;
      }

      Output* home = nullptr;
      if (view->workspace() != nullptr && view->workspace()->group() != nullptr) {
        home = view->workspace()->group()->output();
      }
      // Views owned by another output must not participate in this pass.
      if (home != nullptr && home != rendering) {
        wlr_scene_node_set_enabled(&view->sceneTree()->node, false);
        continue;
      }
      if (!view->onActiveWorkspace()) {
        wlr_scene_node_set_enabled(&view->sceneTree()->node, false);
        continue;
      }

      if (view->tiled() && view->workspace() != nullptr) {
        view->workspace()->syncViewPresentation(view);
        continue;
      }

      const wlr_box& geometry = view->toplevel()->base->geometry;
      const wlr_box target{
          .x = view->sceneTree()->node.x,
          .y = view->sceneTree()->node.y,
          .width = geometry.width,
          .height = geometry.height,
      };
      wlr_box intersection{};
      if (!wlr_box_intersection(&intersection, &target, &outputBox)) {
        wlr_scene_node_set_enabled(&view->sceneTree()->node, false);
        continue;
      }
      wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
      view->setOutputClip(&intersection, target, outputBox);
    }
  }

  void Server::restoreSceneVisibility() {
    View* grabbed = nullptr;
    if (m_cursor != nullptr && (m_cursor->mode() == CursorMode::MoveTile || m_cursor->mode() == CursorMode::Move)) {
      grabbed = m_cursor->grabbedView();
    }

    for (const auto& entry : m_views) {
      View* view = entry.get();
      if (view == nullptr || !view->mapped() || view->sceneTree() == nullptr) {
        continue;
      }
      if (!view->onActiveWorkspace() && view != grabbed) {
        wlr_scene_node_set_enabled(&view->sceneTree()->node, false);
        continue;
      }

      if (view == grabbed) {
        wlr_output* wlrOutput = wlr_output_layout_output_at(m_outputLayout, m_cursor->wlr()->x, m_cursor->wlr()->y);
        Output* output = outputFromWlr(wlrOutput);
        if (output == nullptr) {
          wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
          continue;
        }
        wlr_box outputBox{};
        wlr_output_layout_get_box(m_outputLayout, output->wlr(), &outputBox);
        const wlr_box& geometry = view->toplevel()->base->geometry;
        const wlr_box target{
            .x = view->sceneTree()->node.x,
            .y = view->sceneTree()->node.y,
            .width = geometry.width,
            .height = geometry.height,
        };
        const int border = view->tiled() ? config().appearance.totalBorderWidth() : 0;
        wlr_box decorated = target;
        decorated.x -= border;
        decorated.y -= border;
        decorated.width += 2 * border;
        decorated.height += 2 * border;
        wlr_box intersection{};
        if (!wlr_box_intersection(&intersection, &decorated, &outputBox)) {
          wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
          continue;
        }
        wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
        view->setOutputClip(&intersection, target, outputBox);
        continue;
      }

      if (view->tiled() && view->workspace() != nullptr) {
        view->workspace()->syncViewPresentation(view);
        continue;
      }

      const wlr_box& geometry = view->toplevel()->base->geometry;
      const wlr_box target{
          .x = view->sceneTree()->node.x,
          .y = view->sceneTree()->node.y,
          .width = geometry.width,
          .height = geometry.height,
      };
      wlr_output* wlrOutput =
          wlr_output_layout_output_at(m_outputLayout, target.x + target.width / 2.0, target.y + target.height / 2.0);
      Output* output = outputFromWlr(wlrOutput);
      if (output == nullptr) {
        wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
        continue;
      }
      wlr_box outputBox{};
      wlr_output_layout_get_box(m_outputLayout, output->wlr(), &outputBox);
      wlr_box intersection{};
      if (!wlr_box_intersection(&intersection, &target, &outputBox)) {
        wlr_scene_node_set_enabled(&view->sceneTree()->node, false);
        continue;
      }
      wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
      view->setOutputClip(&intersection, target, outputBox);
    }
  }

} // namespace umbriel
