#include "input/cursor.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "output/output.h"
#include "scene/node.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <cstdlib>

namespace umbriel {

  void Server::focusView(View* view) {
    if (view == nullptr || m_sessionLocked) {
      return;
    }
    // Exclusive layer-shell keyboard grab must not be stolen by window focus.
    if (exclusiveKeyboardLayer() != nullptr) {
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
    if (!m_views.empty() && m_views.front()->mapped()) {
      focusView(m_views.front().get());
      return;
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

  bool Server::handleKeybind(uint32_t keysym) {
    if (m_sessionLocked) {
      return false;
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
        auto current = std::move(m_views.front());
        m_views.erase(m_views.begin());
        m_views.push_back(std::move(current));
        focusView(m_views.front().get());
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
