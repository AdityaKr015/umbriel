#include "view/view.h"

#include "input/cursor.h"
#include "input/seat.h"
#include "server/server.h"
#include "wlr.h"

namespace umbriel {

  View::View(Server& server, wlr_xdg_toplevel* toplevel)
      : SceneNode(SceneNodeKind::View), m_server(&server), m_toplevel(toplevel) {
    m_sceneTree = wlr_scene_xdg_surface_create(m_server->xdgTree(), m_toplevel->base);
    m_sceneTree->node.data = this;
    m_toplevel->base->data = m_sceneTree;

    m_map.notify = onMap;
    wl_signal_add(&m_toplevel->base->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_toplevel->base->surface->events.unmap, &m_unmap);
    m_commit.notify = onCommit;
    wl_signal_add(&m_toplevel->base->surface->events.commit, &m_commit);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_toplevel->events.destroy, &m_destroy);

    m_requestMove.notify = onRequestMove;
    wl_signal_add(&m_toplevel->events.request_move, &m_requestMove);
    m_requestResize.notify = onRequestResize;
    wl_signal_add(&m_toplevel->events.request_resize, &m_requestResize);
    m_requestMaximize.notify = onRequestMaximize;
    wl_signal_add(&m_toplevel->events.request_maximize, &m_requestMaximize);
    m_requestFullscreen.notify = onRequestFullscreen;
    wl_signal_add(&m_toplevel->events.request_fullscreen, &m_requestFullscreen);
    m_setTitle.notify = onSetTitle;
    wl_signal_add(&m_toplevel->events.set_title, &m_setTitle);
    m_setAppId.notify = onSetAppId;
    wl_signal_add(&m_toplevel->events.set_app_id, &m_setAppId);

    if (wlr_foreign_toplevel_manager_v1* manager = m_server->foreignToplevelManager()) {
      m_foreign = wlr_foreign_toplevel_handle_v1_create(manager);
      if (m_foreign != nullptr) {
        m_foreign->data = this;
        m_foreignActivate.notify = onForeignActivate;
        wl_signal_add(&m_foreign->events.request_activate, &m_foreignActivate);
        m_foreignClose.notify = onForeignClose;
        wl_signal_add(&m_foreign->events.request_close, &m_foreignClose);
        m_foreignDestroy.notify = onForeignDestroy;
        wl_signal_add(&m_foreign->events.destroy, &m_foreignDestroy);
        updateForeignIdentity();
        updateForeignState();
      }
    }
  }

  View::~View() {
    if (m_map.link.next != nullptr) {
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_destroy.link);
      wl_list_remove(&m_requestMove.link);
      wl_list_remove(&m_requestResize.link);
      wl_list_remove(&m_requestMaximize.link);
      wl_list_remove(&m_requestFullscreen.link);
      wl_list_remove(&m_setTitle.link);
      wl_list_remove(&m_setAppId.link);
    }
    if (m_foreign != nullptr) {
      leaveForeignOutput();
      wlr_foreign_toplevel_handle_v1_destroy(m_foreign);
      m_foreign = nullptr;
    }
  }

  void View::focus() {
    if (m_server->sessionLocked() || m_server->exclusiveKeyboardLayer() != nullptr) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* surface = m_toplevel->base->surface;
    wlr_surface* prev = seat->keyboard_state.focused_surface;
    if (prev == surface) {
      setForeignActivated(true);
      return;
    }

    if (prev != nullptr) {
      if (wlr_xdg_toplevel* prevToplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev)) {
        wlr_xdg_toplevel_set_activated(prevToplevel, false);
      }
    }

    wlr_scene_node_raise_to_top(&m_sceneTree->node);
    wlr_xdg_toplevel_set_activated(m_toplevel, true);
    setForeignActivated(true);

    if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
      wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
  }

  void View::onMap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void View::onUnmap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_unmap);
    self->handleUnmap();
  }

  void View::onCommit(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_commit);
    self->handleCommit();
  }

  void View::onDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void View::onRequestMove(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestMove);
    self->handleRequestMove();
  }

  void View::onRequestResize(wl_listener* listener, void* data) {
    View* self = wl_container_of(listener, self, m_requestResize);
    self->handleRequestResize(data);
  }

  void View::onRequestMaximize(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestMaximize);
    self->handleRequestMaximize();
  }

  void View::onRequestFullscreen(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestFullscreen);
    self->handleRequestFullscreen();
  }

  void View::onSetTitle(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setTitle);
    self->handleSetTitle();
  }

  void View::onSetAppId(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setAppId);
    self->handleSetAppId();
  }

  void View::placeInUsableArea() {
    wlr_box usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }

    wlr_box* geo = &m_toplevel->base->geometry;
    int width = geo->width > 0 ? geo->width : usable.width;
    int height = geo->height > 0 ? geo->height : usable.height;
    if (width > usable.width) {
      width = usable.width;
    }
    if (height > usable.height) {
      height = usable.height;
    }

    if (width != geo->width || height != geo->height) {
      wlr_xdg_toplevel_set_size(m_toplevel, width, height);
    }
    wlr_scene_node_set_position(&m_sceneTree->node, usable.x, usable.y);
  }

  void View::handleMap() {
    m_mapped = true;
    placeInUsableArea();
    enterForeignOutput();
    updateForeignIdentity();
    updateForeignState();
    if (!m_server->sessionLocked()) {
      m_server->focusView(this);
    }
  }

  void View::handleUnmap() {
    m_mapped = false;
    leaveForeignOutput();
    setForeignActivated(false);
    if (m_server->cursor()->mode() != CursorMode::Passthrough) {
      m_server->cursor()->resetMode();
    }
  }

  void View::handleCommit() {
    if (m_toplevel->base->initial_commit) {
      wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
    }
    updateForeignState();
  }

  void View::handleDestroy() {
    leaveForeignOutput();
    if (m_foreign != nullptr) {
      wl_list_remove(&m_foreignActivate.link);
      wl_list_remove(&m_foreignClose.link);
      wl_list_remove(&m_foreignDestroy.link);
      wlr_foreign_toplevel_handle_v1_destroy(m_foreign);
      m_foreign = nullptr;
    }

    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_requestMove.link);
    wl_list_remove(&m_requestResize.link);
    wl_list_remove(&m_requestMaximize.link);
    wl_list_remove(&m_requestFullscreen.link);
    wl_list_remove(&m_setTitle.link);
    wl_list_remove(&m_setAppId.link);
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_commit.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_requestMove.link.next = nullptr;
    m_requestResize.link.next = nullptr;
    m_requestMaximize.link.next = nullptr;
    m_requestFullscreen.link.next = nullptr;
    m_setTitle.link.next = nullptr;
    m_setAppId.link.next = nullptr;
    m_server->removeView(this);
  }

  void View::handleRequestMove() { m_server->cursor()->beginInteractive(this, CursorMode::Move, 0); }

  void View::handleRequestResize(void* data) {
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    m_server->cursor()->beginInteractive(this, CursorMode::Resize, event->edges);
  }

  void View::handleRequestMaximize() {
    if (!m_toplevel->base->initialized) {
      return;
    }

    const bool maximized = !m_toplevel->current.maximized;
    if (maximized) {
      wlr_box usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
      if (usable.width > 0 && usable.height > 0) {
        wlr_xdg_toplevel_set_size(m_toplevel, usable.width, usable.height);
        wlr_scene_node_set_position(&m_sceneTree->node, usable.x, usable.y);
      }
    }
    wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
    updateForeignState();
  }

  void View::handleRequestFullscreen() {
    if (!m_toplevel->base->initialized) {
      return;
    }

    const bool fullscreen = !m_toplevel->current.fullscreen;
    if (fullscreen) {
      wlr_output* output = m_server->preferredOutput();
      wlr_box fullArea{};
      wlr_output_layout_get_box(m_server->outputLayout(), output, &fullArea);
      if (fullArea.width > 0 && fullArea.height > 0) {
        wlr_xdg_toplevel_set_size(m_toplevel, fullArea.width, fullArea.height);
        wlr_scene_node_set_position(&m_sceneTree->node, fullArea.x, fullArea.y);
      }
    }
    wlr_xdg_toplevel_set_fullscreen(m_toplevel, fullscreen);
    updateForeignState();
  }

} // namespace umbriel
