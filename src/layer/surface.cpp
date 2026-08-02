#include "layer/surface.h"

#include "core/log.h"
#include "input/seat.h"
#include "output/output.h"
#include "server/server.h"
#include "view/popup.h"
#include "wlr.h"

namespace umbriel {

namespace {
constexpr Logger kLog("layer");
} // namespace

LayerSurface::LayerSurface(Server& server, wlr_layer_surface_v1* layerSurface)
    : SceneNode(SceneNodeKind::LayerSurface), m_server(&server), m_layerSurface(layerSurface) {
  if (m_layerSurface->output == nullptr) {
    m_layerSurface->output = m_server->preferredOutput();
  }
  if (m_layerSurface->output == nullptr) {
    kLog.error("no output for layer surface, destroying");
    wlr_layer_surface_v1_destroy(m_layerSurface);
    m_layerSurface = nullptr;
    return;
  }

  Output* out = output();
  if (out == nullptr) {
    kLog.error("layer surface output has no Output wrapper, destroying");
    wlr_layer_surface_v1_destroy(m_layerSurface);
    m_layerSurface = nullptr;
    return;
  }

  const auto layer = m_layerSurface->pending.layer;
  kLog.debug(
      "new layer surface layer={} output={} exclusive={}",
      static_cast<unsigned>(layer),
      m_layerSurface->output->name,
      m_layerSurface->pending.exclusive_zone);

  m_scene = wlr_scene_layer_surface_v1_create(out->layerTree(layer), m_layerSurface);
  if (m_scene == nullptr) {
    kLog.error("failed to create scene layer surface");
    wlr_layer_surface_v1_destroy(m_layerSurface);
    m_layerSurface = nullptr;
    return;
  }
  m_scene->tree->node.data = this;
  m_layerSurface->data = this;

  m_map.notify = onMap;
  wl_signal_add(&m_layerSurface->surface->events.map, &m_map);
  m_unmap.notify = onUnmap;
  wl_signal_add(&m_layerSurface->surface->events.unmap, &m_unmap);
  m_commit.notify = onCommit;
  wl_signal_add(&m_layerSurface->surface->events.commit, &m_commit);
  m_destroy.notify = onDestroy;
  wl_signal_add(&m_layerSurface->events.destroy, &m_destroy);
  m_newPopup.notify = onNewPopup;
  wl_signal_add(&m_layerSurface->events.new_popup, &m_newPopup);
}

LayerSurface::~LayerSurface() {
  if (m_map.link.next != nullptr) {
    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_newPopup.link);
  }
}

Output* LayerSurface::output() const {
  if (m_layerSurface == nullptr || m_layerSurface->output == nullptr) {
    return nullptr;
  }
  return static_cast<Output*>(m_layerSurface->output->data);
}

bool LayerSurface::exclusiveKeyboard() const {
  return m_mapped && m_layerSurface != nullptr
      && m_layerSurface->current.keyboard_interactive
      == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

bool LayerSurface::acceptsKeyboard() const {
  if (!m_mapped || m_layerSurface == nullptr) {
    return false;
  }
  const auto interactivity = m_layerSurface->current.keyboard_interactive;
  return interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
      || interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
}

bool LayerSurface::hasKeyboardFocus() const {
  if (m_layerSurface == nullptr) {
    return false;
  }
  return m_server->seat()->wlr()->keyboard_state.focused_surface == m_layerSurface->surface;
}

void LayerSurface::reparentToLayer(uint32_t layer) {
  Output* out = output();
  if (out == nullptr || m_scene == nullptr) {
    return;
  }
  wlr_scene_node_reparent(&m_scene->tree->node, out->layerTree(layer));
}

void LayerSurface::focus() {
  if (m_server->sessionLocked() || !acceptsKeyboard()) {
    return;
  }

  wlr_seat* seat = m_server->seat()->wlr();
  wlr_surface* surface = m_layerSurface->surface;
  wlr_surface* prev = seat->keyboard_state.focused_surface;
  if (prev == surface) {
    return;
  }

  if (prev != nullptr) {
    if (wlr_xdg_toplevel* prevToplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev)) {
      wlr_xdg_toplevel_set_activated(prevToplevel, false);
    }
  }

  // Give the layer seat keyboard focus so clients (e.g. Noctalia) receive Escape.
  if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
    wlr_seat_keyboard_notify_enter(
        seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
  } else {
    wlr_seat_keyboard_notify_enter(seat, surface, nullptr, 0, nullptr);
  }
}

void LayerSurface::unconstrainPopup(wlr_xdg_popup* popup) {
  Output* out = output();
  if (out == nullptr || m_scene == nullptr) {
    return;
  }

  int width = 0;
  int height = 0;
  wlr_output_effective_resolution(m_layerSurface->output, &width, &height);

  // Box is relative to the layer surface origin (output-local scene coords).
  wlr_box box = {
      .x = -m_scene->tree->node.x,
      .y = -m_scene->tree->node.y,
      .width = width,
      .height = height,
  };
  wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

void LayerSurface::onMap(wl_listener* listener, void* /*data*/) {
  LayerSurface* self = wl_container_of(listener, self, m_map);
  self->handleMap();
}

void LayerSurface::onUnmap(wl_listener* listener, void* /*data*/) {
  LayerSurface* self = wl_container_of(listener, self, m_unmap);
  self->handleUnmap();
}

void LayerSurface::onCommit(wl_listener* listener, void* /*data*/) {
  LayerSurface* self = wl_container_of(listener, self, m_commit);
  self->handleCommit();
}

void LayerSurface::onDestroy(wl_listener* listener, void* /*data*/) {
  LayerSurface* self = wl_container_of(listener, self, m_destroy);
  self->handleDestroy();
}

void LayerSurface::onNewPopup(wl_listener* listener, void* data) {
  LayerSurface* self = wl_container_of(listener, self, m_newPopup);
  self->handleNewPopup(data);
}

void LayerSurface::handleMap() {
  m_mapped = true;
  if (Output* out = output()) {
    out->arrangeLayers();
  }
  // Protocol: exclusive must receive keyboard focus. On-demand is click-to-focus.
  if (exclusiveKeyboard()) {
    focus();
  }
}

void LayerSurface::handleUnmap() {
  const bool hadFocus = hasKeyboardFocus();
  m_mapped = false;
  // Avoid sending configures while unmapping (wrong serial / client abort).
  m_arrangingOut = true;
  if (Output* out = output()) {
    out->arrangeLayers();
  }
  m_arrangingOut = false;
  if (hadFocus) {
    m_server->refocus();
  }
}

void LayerSurface::handleCommit() {
  if (m_layerSurface->initial_commit) {
    if (Output* out = output()) {
      out->arrangeLayers();
    }
    return;
  }

  if ((m_layerSurface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) != 0) {
    reparentToLayer(m_layerSurface->current.layer);
  }

  if ((m_layerSurface->current.committed & WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY)
      != 0) {
    if (exclusiveKeyboard()) {
      focus();
    } else if (hasKeyboardFocus() && !acceptsKeyboard()) {
      m_server->refocus();
    }
  }

  if (m_layerSurface->current.committed != 0) {
    if (Output* out = output()) {
      out->arrangeLayers();
    }
  }
}

void LayerSurface::handleDestroy() {
  Server* server = m_server;
  wlr_output* wlrOutput = m_layerSurface != nullptr ? m_layerSurface->output : nullptr;
  kLog.debug("destroy layer surface output={}", wlrOutput != nullptr ? wlrOutput->name : "(none)");

  wl_list_remove(&m_map.link);
  wl_list_remove(&m_unmap.link);
  wl_list_remove(&m_commit.link);
  wl_list_remove(&m_destroy.link);
  wl_list_remove(&m_newPopup.link);
  m_map.link.next = nullptr;
  m_unmap.link.next = nullptr;
  m_commit.link.next = nullptr;
  m_destroy.link.next = nullptr;
  m_newPopup.link.next = nullptr;

  // removeLayerSurface deletes this. Arrange after erase, never via this->m_server.
  server->removeLayerSurface(this, wlrOutput);
}

void LayerSurface::handleNewPopup(void* data) {
  auto* popup = static_cast<wlr_xdg_popup*>(data);
  // Keep popup scene under the layer surface so geometry stays surface-local.
  new Popup(popup, m_scene->tree);
}

} // namespace umbriel
