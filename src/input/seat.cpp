#include "input/seat.h"

#include "input/cursor.h"
#include "server/server.h"
#include "wlr.h"

namespace umbriel {

  Seat::Seat(Server& server) : m_server(&server) {
    m_seat = wlr_seat_create(m_server->display(), "seat0");

    m_cursorShapeManager = wlr_cursor_shape_manager_v1_create(m_server->display(), 2);
    m_requestSetShape.notify = onRequestSetShape;
    wl_signal_add(&m_cursorShapeManager->events.request_set_shape, &m_requestSetShape);

    m_requestCursor.notify = onRequestCursor;
    wl_signal_add(&m_seat->events.request_set_cursor, &m_requestCursor);

    m_pointerFocusChange.notify = onPointerFocusChange;
    wl_signal_add(&m_seat->pointer_state.events.focus_change, &m_pointerFocusChange);

    m_requestSetSelection.notify = onRequestSetSelection;
    wl_signal_add(&m_seat->events.request_set_selection, &m_requestSetSelection);
  }

  Seat::~Seat() {
    wl_list_remove(&m_requestCursor.link);
    wl_list_remove(&m_requestSetShape.link);
    wl_list_remove(&m_pointerFocusChange.link);
    wl_list_remove(&m_requestSetSelection.link);
  }

  void Seat::updateCapabilities(bool hasKeyboard) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (hasKeyboard) {
      caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(m_seat, caps);
  }

  void Seat::onRequestCursor(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestCursor);
    self->handleRequestCursor(data);
  }

  void Seat::onRequestSetShape(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestSetShape);
    self->handleRequestSetShape(data);
  }

  void Seat::onPointerFocusChange(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_pointerFocusChange);
    self->handlePointerFocusChange(data);
  }

  void Seat::onRequestSetSelection(wl_listener* listener, void* data) {
    Seat* self = wl_container_of(listener, self, m_requestSetSelection);
    self->handleRequestSetSelection(data);
  }

  void Seat::handleRequestCursor(void* data) {
    auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
    if (m_seat->pointer_state.focused_client == event->seat_client) {
      wlr_cursor_set_surface(m_server->cursor()->wlr(), event->surface, event->hotspot_x, event->hotspot_y);
    }
  }

  void Seat::handleRequestSetShape(void* data) {
    auto* event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
      return;
    }
    if (m_seat->pointer_state.focused_client != event->seat_client) {
      return;
    }

    const char* name = wlr_cursor_shape_v1_name(event->shape);
    if (name == nullptr) {
      return;
    }
    wlr_cursor_set_xcursor(m_server->cursor()->wlr(), m_server->cursor()->xcursorManager(), name);
  }

  void Seat::handlePointerFocusChange(void* data) {
    auto* event = static_cast<wlr_seat_pointer_focus_change_event*>(data);
    if (event->new_surface == nullptr) {
      wlr_cursor_set_xcursor(m_server->cursor()->wlr(), m_server->cursor()->xcursorManager(), "default");
    }
  }

  void Seat::handleRequestSetSelection(void* data) {
    auto* event = static_cast<wlr_seat_request_set_selection_event*>(data);
    wlr_seat_set_selection(m_seat, event->source, event->serial);
  }

} // namespace umbriel
