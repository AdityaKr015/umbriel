#include "view/view.h"

#include "input/cursor.h"
#include "input/seat.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "server/server.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {
  namespace {
    bool looksTiled(const wlr_xdg_toplevel* toplevel) {
      const auto& state = toplevel->current;
      const bool fixedWidth = state.max_width > 0 && state.min_width == state.max_width;
      const bool fixedHeight = state.max_height > 0 && state.min_height == state.max_height;
      return toplevel->parent == nullptr && !fixedWidth && !fixedHeight;
    }
  } // namespace

  View::View(Server& server, wlr_xdg_toplevel* toplevel)
      : SceneNode(SceneNodeKind::View), m_server(&server), m_toplevel(toplevel) {
    m_sceneTree = wlr_scene_xdg_surface_create(m_server->xdgTree(), m_toplevel->base);
    m_sceneTree->node.data = this;
    m_toplevel->base->data = m_sceneTree;
    wlr_scene_node_set_enabled(&m_sceneTree->node, false);

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
    cancelPositionAnimation();
    setWorkspace(nullptr);
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

  void View::setWorkspace(Workspace* workspace) {
    if (m_workspace == workspace) {
      return;
    }
    if (m_workspace != nullptr) {
      m_workspace->removeView(this);
    }
    m_workspace = workspace;
    if (m_workspace != nullptr) {
      m_workspace->addView(this);
    } else {
      setOnActiveWorkspace(true);
    }
    if (m_mapped && m_onActiveWorkspace) {
      enterForeignOutput();
    }
  }

  void View::detachWorkspace() {
    m_workspace = nullptr;
    setOnActiveWorkspace(true);
  }

  void View::setOnActiveWorkspace(bool active) {
    if (m_onActiveWorkspace == active) {
      return;
    }
    m_onActiveWorkspace = active;
    if (m_sceneTree != nullptr) {
      wlr_scene_node_set_enabled(&m_sceneTree->node, active);
    }
    if (!m_mapped) {
      return;
    }
    if (active) {
      enterForeignOutput();
    } else {
      leaveForeignOutput();
      setForeignActivated(false);
    }
  }

  void View::focus() {
    if (m_server->sessionLocked() || m_server->exclusiveKeyboardLayer() != nullptr) {
      return;
    }
    if (!m_onActiveWorkspace) {
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

  void View::cancelPositionAnimation() {
    if (m_posAnim != 0) {
      m_server->animator().cancel(m_posAnim);
      m_posAnim = 0;
    }
  }

  void View::setPosition(int x, int y) {
    cancelPositionAnimation();
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
  }

  void View::animateTo(int x, int y) {
    if (!m_mapped || !m_onActiveWorkspace) {
      setPosition(x, y);
      return;
    }
    const int fromX = m_sceneTree->node.x;
    const int fromY = m_sceneTree->node.y;
    if (fromX == x && fromY == y) {
      cancelPositionAnimation();
      return;
    }
    cancelPositionAnimation();
    m_posAnim = m_server->animator().animate(
        0.0, 1.0, kAnimMs,
        [this, fromX, fromY, x, y](double progress) {
          wlr_scene_node_set_position(
              &m_sceneTree->node, static_cast<int>(std::lround(fromX + (x - fromX) * progress)),
              static_cast<int>(std::lround(fromY + (y - fromY) * progress))
          );
        },
        [this] { m_posAnim = 0; }
    );
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      wlr_output_schedule_frame(m_workspace->group()->output()->wlr());
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
    const int width = std::clamp(geo->width > 0 ? geo->width : usable.width, 1, usable.width);
    const int height = std::clamp(geo->height > 0 ? geo->height : usable.height, 1, usable.height);
    if (width != geo->width || height != geo->height) {
      wlr_xdg_toplevel_set_size(m_toplevel, width, height);
    }
    const int x = usable.x + (usable.width - width) / 2;
    const int y = usable.y + (usable.height - height) / 2;
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
  }

  void View::handleMap() {
    m_mapped = true;
    m_tiled = looksTiled(m_toplevel);
    if (m_workspace != nullptr) {
      m_workspace->layoutAttach(this);
    } else if (Output* out = m_server->outputFromWlr(m_server->preferredOutput())) {
      if (WorkspaceGroup* group = out->workspaceGroup()) {
        setWorkspace(group->active());
      } else {
        setOnActiveWorkspace(true);
      }
    } else {
      setOnActiveWorkspace(true);
    }
    if (!m_tiled) {
      placeInUsableArea();
    }
    updateForeignIdentity();
    updateForeignState();
    if (m_onActiveWorkspace && !m_server->sessionLocked()) {
      m_server->focusView(this);
    }
  }

  void View::handleUnmap() {
    cancelPositionAnimation();
    m_mapped = false;
    if (m_workspace != nullptr) {
      m_workspace->layoutDetach(this);
    }
    leaveForeignOutput();
    setForeignActivated(false);
    if (m_server->cursor()->mode() != CursorMode::Passthrough) {
      m_server->cursor()->resetMode();
    }
  }

  void View::handleCommit() {
    if (m_toplevel->base->initial_commit) {
      if (looksTiled(m_toplevel)) {
        const wlr_box usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
        const int viewportWidth = std::max(1, usable.width - 2 * kGap);
        const int height = std::max(1, usable.height - 2 * kGap);
        wlr_xdg_toplevel_set_size(
            m_toplevel, std::max(1, static_cast<int>(std::lround(kDefaultWidthFrac * viewportWidth))), height
        );
      } else {
        wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
      }
    }
    updateForeignState();
  }

  void View::handleDestroy() {
    cancelPositionAnimation();
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

  void View::handleRequestMove() {
    m_server->cursor()->beginInteractive(this, m_tiled ? CursorMode::MoveTile : CursorMode::Move, 0);
  }

  void View::handleRequestResize(void* data) {
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    m_server->cursor()->beginInteractive(this, m_tiled ? CursorMode::ResizeTile : CursorMode::Resize, event->edges);
  }

  void View::handleRequestMaximize() {
    if (!m_toplevel->base->initialized) {
      return;
    }

    if (m_tiled && m_workspace != nullptr) {
      const int column = m_workspace->layout().columnOf(this);
      const bool maximized = m_workspace->layout().toggleFullWidth(column);
      wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
      m_workspace->ensureFocusedVisible();
      m_workspace->arrange();
      updateForeignState();
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
    if (!fullscreen && m_workspace != nullptr) {
      m_workspace->arrange();
    }
    updateForeignState();
  }

} // namespace umbriel
