#include "view/view.h"

#include "input/cursor.h"
#include "input/seat.h"
#include "server/server.h"
#include "wlr.h"

namespace umbriel {

View::View(Server& server, wlr_xdg_toplevel* toplevel) : m_server(&server), m_toplevel(toplevel) {
  m_sceneTree = wlr_scene_xdg_surface_create(&m_server->scene()->tree, m_toplevel->base);
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
  }
}

void View::focus() {
  wlr_seat* seat = m_server->seat()->wlr();
  wlr_surface* surface = m_toplevel->base->surface;
  wlr_surface* prev = seat->keyboard_state.focused_surface;
  if (prev == surface) {
    return;
  }

  if (prev != nullptr) {
    if (wlr_xdg_toplevel* prevToplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev)) {
      wlr_xdg_toplevel_set_activated(prevToplevel, false);
    }
  }

  wlr_scene_node_raise_to_top(&m_sceneTree->node);
  wlr_xdg_toplevel_set_activated(m_toplevel, true);

  if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
    wlr_seat_keyboard_notify_enter(
        seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
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
  self->handleRequestConfigure();
}

void View::onRequestFullscreen(wl_listener* listener, void* /*data*/) {
  View* self = wl_container_of(listener, self, m_requestFullscreen);
  self->handleRequestConfigure();
}

void View::handleMap() {
  m_mapped = true;
  focus();
}

void View::handleUnmap() {
  m_mapped = false;
  if (m_server->cursor()->mode() != CursorMode::Passthrough) {
    m_server->cursor()->resetMode();
  }
}

void View::handleCommit() {
  if (m_toplevel->base->initial_commit) {
    wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
  }
}

void View::handleDestroy() {
  wl_list_remove(&m_map.link);
  wl_list_remove(&m_unmap.link);
  wl_list_remove(&m_commit.link);
  wl_list_remove(&m_destroy.link);
  wl_list_remove(&m_requestMove.link);
  wl_list_remove(&m_requestResize.link);
  wl_list_remove(&m_requestMaximize.link);
  wl_list_remove(&m_requestFullscreen.link);
  m_map.link.next = nullptr;
  m_unmap.link.next = nullptr;
  m_commit.link.next = nullptr;
  m_destroy.link.next = nullptr;
  m_requestMove.link.next = nullptr;
  m_requestResize.link.next = nullptr;
  m_requestMaximize.link.next = nullptr;
  m_requestFullscreen.link.next = nullptr;
  m_server->removeView(this);
}

void View::handleRequestMove() {
  m_server->cursor()->beginInteractive(this, CursorMode::Move, 0);
}

void View::handleRequestResize(void* data) {
  auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
  m_server->cursor()->beginInteractive(this, CursorMode::Resize, event->edges);
}

void View::handleRequestConfigure() {
  if (m_toplevel->base->initialized) {
    wlr_xdg_surface_schedule_configure(m_toplevel->base);
  }
}

} // namespace umbriel
