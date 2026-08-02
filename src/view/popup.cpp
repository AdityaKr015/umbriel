#include "view/popup.h"

#include "layer/surface.h"
#include "wlr.h"

#include <cassert>

namespace umbriel {

Popup::Popup(wlr_xdg_popup* popup, wlr_scene_tree* parentTree) : m_popup(popup) {
  if (parentTree == nullptr) {
    wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(m_popup->parent);
    assert(parent != nullptr);
    parentTree = static_cast<wlr_scene_tree*>(parent->data);
  }
  m_popup->base->data = wlr_scene_xdg_surface_create(parentTree, m_popup->base);

  m_commit.notify = onCommit;
  wl_signal_add(&m_popup->base->surface->events.commit, &m_commit);
  m_destroy.notify = onDestroy;
  wl_signal_add(&m_popup->events.destroy, &m_destroy);
}

Popup::~Popup() {
  if (m_commit.link.next != nullptr) {
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_destroy.link);
  }
}

void Popup::onCommit(wl_listener* listener, void* /*data*/) {
  Popup* self = wl_container_of(listener, self, m_commit);
  self->handleCommit();
}

void Popup::onDestroy(wl_listener* listener, void* /*data*/) {
  Popup* self = wl_container_of(listener, self, m_destroy);
  self->handleDestroy();
}

void Popup::handleCommit() {
  if (!m_popup->base->initial_commit) {
    return;
  }

  if (wlr_layer_surface_v1* layer = wlr_layer_surface_v1_try_from_wlr_surface(m_popup->parent)) {
    if (auto* surface = static_cast<LayerSurface*>(layer->data)) {
      surface->unconstrainPopup(m_popup);
    }
  }

  wlr_xdg_surface_schedule_configure(m_popup->base);
}

void Popup::handleDestroy() {
  wl_list_remove(&m_commit.link);
  wl_list_remove(&m_destroy.link);
  m_commit.link.next = nullptr;
  m_destroy.link.next = nullptr;
  delete this;
}

} // namespace umbriel
