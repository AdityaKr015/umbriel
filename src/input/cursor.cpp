#include "input/cursor.h"

#include "input/seat.h"
#include "layer/surface.h"
#include "lock/session_lock.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

namespace umbriel {

Cursor::Cursor(Server& server) : m_server(&server) {
  m_cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(m_cursor, m_server->outputLayout());
  m_xcursorManager = wlr_xcursor_manager_create(nullptr, 24);

  m_motion.notify = onMotion;
  wl_signal_add(&m_cursor->events.motion, &m_motion);
  m_motionAbsolute.notify = onMotionAbsolute;
  wl_signal_add(&m_cursor->events.motion_absolute, &m_motionAbsolute);
  m_button.notify = onButton;
  wl_signal_add(&m_cursor->events.button, &m_button);
  m_axis.notify = onAxis;
  wl_signal_add(&m_cursor->events.axis, &m_axis);
  m_frame.notify = onFrame;
  wl_signal_add(&m_cursor->events.frame, &m_frame);

  m_constraintDestroy.link.next = nullptr;
}

Cursor::~Cursor() {
  if (m_constraintDestroy.link.next != nullptr) {
    wl_list_remove(&m_constraintDestroy.link);
  }
  wl_list_remove(&m_motion.link);
  wl_list_remove(&m_motionAbsolute.link);
  wl_list_remove(&m_button.link);
  wl_list_remove(&m_axis.link);
  wl_list_remove(&m_frame.link);
  wlr_xcursor_manager_destroy(m_xcursorManager);
  wlr_cursor_destroy(m_cursor);
}

void Cursor::attachInputDevice(wlr_input_device* device) {
  wlr_cursor_attach_input_device(m_cursor, device);
}

void Cursor::beginInteractive(View* view, CursorMode mode, uint32_t edges) {
  setActiveConstraint(nullptr);
  m_grabbedView = view;
  m_mode = mode;
  m_resizeEdges = edges;

  if (mode == CursorMode::Move) {
    m_grabX = m_cursor->x - view->sceneTree()->node.x;
    m_grabY = m_cursor->y - view->sceneTree()->node.y;
    return;
  }

  wlr_box* geo = &view->toplevel()->base->geometry;
  double borderX = (view->sceneTree()->node.x + geo->x) + ((edges & WLR_EDGE_RIGHT) != 0 ? geo->width : 0);
  double borderY = (view->sceneTree()->node.y + geo->y) + ((edges & WLR_EDGE_BOTTOM) != 0 ? geo->height : 0);
  m_grabX = m_cursor->x - borderX;
  m_grabY = m_cursor->y - borderY;
  m_grabGeoX = geo->x + static_cast<int>(view->sceneTree()->node.x);
  m_grabGeoY = geo->y + static_cast<int>(view->sceneTree()->node.y);
  m_grabGeoWidth = geo->width;
  m_grabGeoHeight = geo->height;
}

void Cursor::resetMode() {
  m_mode = CursorMode::Passthrough;
  m_grabbedView = nullptr;
}

void Cursor::onMotion(wl_listener* listener, void* data) {
  Cursor* self = wl_container_of(listener, self, m_motion);
  self->handleMotion(data);
}

void Cursor::onMotionAbsolute(wl_listener* listener, void* data) {
  Cursor* self = wl_container_of(listener, self, m_motionAbsolute);
  self->handleMotionAbsolute(data);
}

void Cursor::onButton(wl_listener* listener, void* data) {
  Cursor* self = wl_container_of(listener, self, m_button);
  self->handleButton(data);
}

void Cursor::onAxis(wl_listener* listener, void* data) {
  Cursor* self = wl_container_of(listener, self, m_axis);
  self->handleAxis(data);
}

void Cursor::onFrame(wl_listener* listener, void* /*data*/) {
  Cursor* self = wl_container_of(listener, self, m_frame);
  self->handleFrame();
}

void Cursor::handleMotion(void* data) {
  auto* event = static_cast<wlr_pointer_motion_event*>(data);
  m_server->notifyIdleActivity();

  wlr_relative_pointer_manager_v1_send_relative_motion(
      m_server->relativePointerManager(),
      m_server->seat()->wlr(),
      static_cast<uint64_t>(event->time_msec) * 1000,
      event->delta_x,
      event->delta_y,
      event->unaccel_dx,
      event->unaccel_dy);

  if (m_activeConstraint != nullptr
      && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
    return;
  }

  double dx = event->delta_x;
  double dy = event->delta_y;
  if (m_activeConstraint != nullptr
      && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
    if (!confineDelta(&dx, &dy)) {
      return;
    }
  }

  wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
  processMotion(event->time_msec);
}

void Cursor::handleMotionAbsolute(void* data) {
  auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
  m_server->notifyIdleActivity();
  if (m_activeConstraint != nullptr
      && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
    return;
  }

  double lx = 0;
  double ly = 0;
  wlr_cursor_absolute_to_layout_coords(m_cursor, &event->pointer->base, event->x, event->y, &lx, &ly);

  if (m_activeConstraint != nullptr
      && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
    double dx = lx - m_cursor->x;
    double dy = ly - m_cursor->y;
    if (!confineDelta(&dx, &dy)) {
      return;
    }
    wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
  } else {
    wlr_cursor_warp_absolute(m_cursor, &event->pointer->base, event->x, event->y);
  }
  processMotion(event->time_msec);
}

void Cursor::handleButton(void* data) {
  auto* event = static_cast<wlr_pointer_button_event*>(data);
  m_server->notifyIdleActivity();
  wlr_seat_pointer_notify_button(m_server->seat()->wlr(), event->time_msec, event->button, event->state);

  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    resetMode();
    return;
  }

  if (m_server->sessionLocked()) {
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    if (surface != nullptr) {
      if (wlr_session_lock_surface_v1* lockSurface =
              wlr_session_lock_surface_v1_try_from_wlr_surface(surface)) {
        if (auto* node = static_cast<LockSurface*>(lockSurface->data)) {
          node->focus();
        }
      }
    }
    return;
  }

  double sx = 0;
  double sy = 0;
  wlr_surface* surface = nullptr;
  LayerSurface* layer = nullptr;
  View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
  if (layer != nullptr) {
    layer->focus();
  } else if (m_server->exclusiveKeyboardLayer() == nullptr) {
    m_server->focusView(view);
  }
}

void Cursor::handleAxis(void* data) {
  auto* event = static_cast<wlr_pointer_axis_event*>(data);
  m_server->notifyIdleActivity();
  wlr_seat_pointer_notify_axis(
      m_server->seat()->wlr(),
      event->time_msec,
      event->orientation,
      event->delta,
      event->delta_discrete,
      event->source,
      event->relative_direction);
}

void Cursor::handleFrame() { wlr_seat_pointer_notify_frame(m_server->seat()->wlr()); }

void Cursor::processMotion(uint32_t timeMsec) {
  if (m_mode == CursorMode::Move) {
    if (m_server->sessionLocked()) {
      resetMode();
    } else {
      processMove();
      return;
    }
  }
  if (m_mode == CursorMode::Resize) {
    if (m_server->sessionLocked()) {
      resetMode();
    } else {
      processResize();
      return;
    }
  }

  double sx = 0;
  double sy = 0;
  wlr_surface* surface = nullptr;
  m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
  wlr_seat* seat = m_server->seat()->wlr();
  if (surface != nullptr) {
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
  } else {
    wlr_cursor_set_xcursor(m_cursor, m_xcursorManager, "default");
    wlr_seat_pointer_clear_focus(seat);
  }
  updateConstraintForSurface(surface);
}

void Cursor::processMove() {
  wlr_scene_node_set_position(
      &m_grabbedView->sceneTree()->node,
      static_cast<int>(m_cursor->x - m_grabX),
      static_cast<int>(m_cursor->y - m_grabY));
}

void Cursor::processResize() {
  double borderX = m_cursor->x - m_grabX;
  double borderY = m_cursor->y - m_grabY;
  int newLeft = m_grabGeoX;
  int newRight = m_grabGeoX + m_grabGeoWidth;
  int newTop = m_grabGeoY;
  int newBottom = m_grabGeoY + m_grabGeoHeight;

  if ((m_resizeEdges & WLR_EDGE_TOP) != 0) {
    newTop = static_cast<int>(borderY);
    if (newTop >= newBottom) {
      newTop = newBottom - 1;
    }
  } else if ((m_resizeEdges & WLR_EDGE_BOTTOM) != 0) {
    newBottom = static_cast<int>(borderY);
    if (newBottom <= newTop) {
      newBottom = newTop + 1;
    }
  }

  if ((m_resizeEdges & WLR_EDGE_LEFT) != 0) {
    newLeft = static_cast<int>(borderX);
    if (newLeft >= newRight) {
      newLeft = newRight - 1;
    }
  } else if ((m_resizeEdges & WLR_EDGE_RIGHT) != 0) {
    newRight = static_cast<int>(borderX);
    if (newRight <= newLeft) {
      newRight = newLeft + 1;
    }
  }

  wlr_box* geo = &m_grabbedView->toplevel()->base->geometry;
  wlr_scene_node_set_position(&m_grabbedView->sceneTree()->node, newLeft - geo->x, newTop - geo->y);
  wlr_xdg_toplevel_set_size(m_grabbedView->toplevel(), newRight - newLeft, newBottom - newTop);
}

} // namespace umbriel
