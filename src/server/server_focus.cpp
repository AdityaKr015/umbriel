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
#include <linux/input-event-codes.h>

namespace umbriel {

  void Server::focusView(View* view) {
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
  }

  LayerSurface* Server::exclusiveKeyboardLayer() const {
    for (const auto& entry : m_layerSurfaces) {
      if (entry->exclusiveKeyboard()) {
        return entry.get();
      }
    }
    return nullptr;
  }

  void Server::refocus() {
    if (m_sessionLocked) {
      return;
    }
    if (LayerSurface* layer = exclusiveKeyboardLayer()) {
      layer->focus();
      return;
    }
    // Do not auto-focus on-demand layers; only exclusive grabs keyboard without a click.
    for (const auto& entry : m_views) {
      if (entry->mapped() && entry->onActiveWorkspace()) {
        focusView(entry.get());
        return;
      }
    }
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

  bool Server::handleKeybind(uint32_t keysym, uint32_t modifiers, uint32_t keycode) {
    if (m_sessionLocked) {
      return false;
    }

    // Use the physical keycode so mod+Shift+1 still resolves to workspace 1
    // (Shift changes the keysym to '!' / '@' / … depending on layout).
    auto workspaceIndex = [](uint32_t code, uint32_t sym) -> int {
      if (code >= KEY_1 && code <= KEY_9) {
        return static_cast<int>(code - KEY_1);
      }
      if (code >= KEY_KP1 && code <= KEY_KP9) {
        return static_cast<int>(code - KEY_KP1);
      }
      if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
        return static_cast<int>(sym - XKB_KEY_1);
      }
      if (sym >= XKB_KEY_KP_1 && sym <= XKB_KEY_KP_9) {
        return static_cast<int>(sym - XKB_KEY_KP_1);
      }
      return -1;
    };

    if (const int index = workspaceIndex(keycode, keysym); index >= 0) {
      Output* out = outputFromWlr(preferredOutput());
      if (out == nullptr || out->workspaceGroup() == nullptr) {
        return true;
      }
      WorkspaceGroup* group = out->workspaceGroup();
      Workspace* target = group->workspaceAt(static_cast<size_t>(index));
      if (target == nullptr) {
        return true;
      }
      if ((modifiers & WLR_MODIFIER_SHIFT) != 0) {
        for (const auto& entry : m_views) {
          if (entry->mapped() && entry->onActiveWorkspace()) {
            entry->setWorkspace(target);
            group->activate(target);
            focusView(entry.get());
            return true;
          }
        }
        group->activateIndex(static_cast<size_t>(index));
        return true;
      }
      group->activateIndex(static_cast<size_t>(index));
      return true;
    }

    switch (keysym) {
    case XKB_KEY_Escape:
      stop();
      return true;
    case XKB_KEY_Return: {
      const char* terminal = std::getenv("TERMINAL");
      if (terminal == nullptr || terminal[0] == '\0') {
        wlr_log(WLR_ERROR, "mod+Return: set TERMINAL to your terminal binary");
        return true;
      }
      spawn(terminal);
      return true;
    }
    case XKB_KEY_F1:
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
    default:
      return false;
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
