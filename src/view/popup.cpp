#include "view/popup.h"

#include "layer/surface.h"
#include "scene/node.h"
#include "view/view.h"
#include "wlr.h"

#include <cassert>

namespace umbriel {

  namespace {
    View* toplevelViewFromSurface(wlr_surface* surface) {
      wlr_surface* walk = surface;
      while (walk != nullptr) {
        if (wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(walk)) {
          auto* tree = static_cast<wlr_scene_tree*>(toplevel->base->data);
          if (tree == nullptr || tree->node.data == nullptr) {
            return nullptr;
          }
          auto* node = static_cast<SceneNode*>(tree->node.data);
          if (node->kind != SceneNodeKind::View) {
            return nullptr;
          }
          return static_cast<View*>(node);
        }
        if (wlr_xdg_popup* popup = wlr_xdg_popup_try_from_wlr_surface(walk)) {
          walk = popup->parent;
          continue;
        }
        break;
      }
      return nullptr;
    }
  } // namespace

  Popup::Popup(wlr_xdg_popup* popup, wlr_scene_tree* parentTree) : m_popup(popup) {
    if (parentTree == nullptr) {
      wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(m_popup->parent);
      assert(parent != nullptr);
      parentTree = static_cast<wlr_scene_tree*>(parent->data);
    }
    m_popup->base->data = wlr_scene_xdg_surface_create(parentTree, m_popup->base);

    m_commit.notify = onCommit;
    wl_signal_add(&m_popup->base->surface->events.commit, &m_commit);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_popup->base->surface->events.unmap, &m_unmap);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_popup->events.destroy, &m_destroy);
  }

  Popup::~Popup() {
    if (m_commit.link.next != nullptr) {
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  void Popup::onCommit(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_commit); // NOLINT(modernize-use-auto)
    self->handleCommit();
  }

  void Popup::onUnmap(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_unmap); // NOLINT(modernize-use-auto)
    self->m_blur.hide();
  }

  void Popup::onDestroy(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_destroy); // NOLINT(modernize-use-auto)
    self->handleDestroy();
  }

  void Popup::handleCommit() {
    const wlr_box& geometry = m_popup->base->geometry;
    m_blur.update(
        static_cast<wlr_scene_tree*>(m_popup->base->data), m_popup->base->surface,
        wlr_box{0, 0, geometry.width, geometry.height}, geometry, 0
    );
    if (!m_popup->base->initial_commit) {
      return;
    }

    if (wlr_layer_surface_v1* layer = wlr_layer_surface_v1_try_from_wlr_surface(m_popup->parent)) {
      if (auto* surface = static_cast<LayerSurface*>(layer->data)) {
        surface->unconstrainPopup(m_popup);
      }
    } else if (View* view = toplevelViewFromSurface(m_popup->parent)) {
      view->unconstrainPopup(m_popup);
    }

    wlr_xdg_surface_schedule_configure(m_popup->base);
  }

  void Popup::handleDestroy() {
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_destroy.link);
    m_commit.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_destroy.link.next = nullptr;
    delete this;
  }

} // namespace umbriel
