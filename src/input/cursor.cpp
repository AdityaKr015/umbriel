#include "input/cursor.h"

#include "config/config.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "layout/drop_target.h"
#include "layout/scrolling.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/hint_rect.h"
#include "server/server.h"
#include "view/view.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include "wlr.h"
// clang-format on
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    // Panels (top/overlay) keep working inside the overview. Wallpaper and
    // bottom-layer widgets are part of the inert desktop behind the filmstrip,
    // so their clicks belong to the overview instead.
    bool overviewPassthroughLayer(const LayerSurface* layer) {
      if (layer == nullptr) {
        return false;
      }
      const uint32_t which = layer->layerSurface()->current.layer;
      return which == ZWLR_LAYER_SHELL_V1_LAYER_TOP || which == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    }

    bool surfaceLocalCoordinates(wlr_scene* scene, wlr_surface* target, double lx, double ly, double* sx, double* sy) {
      if (target == nullptr) {
        return false;
      }

      struct SurfacePosition {
        wlr_surface* target;
        int x = 0;
        int y = 0;
        bool found = false;
      } position{target};

      wlr_scene_node_for_each_buffer(
          &scene->tree.node,
          [](wlr_scene_buffer* buffer, int x, int y, void* data) {
            auto* position = static_cast<SurfacePosition*>(data);
            if (position->found) {
              return;
            }
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface != nullptr && sceneSurface->surface == position->target) {
              position->x = x;
              position->y = y;
              position->found = true;
            }
          },
          &position
      );

      if (!position.found) {
        return false;
      }
      *sx = lx - position.x;
      *sy = ly - position.y;
      return true;
    }

  } // namespace

  Cursor::Cursor(Server& server) : m_server(&server) {
    m_cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(m_cursor, m_server->outputLayout());
    const Config::Input::Cursor& configured = config().input.cursor;
    m_xcursorTheme = configured.theme;
    m_xcursorSize = configured.size;
    m_xcursorManager =
        wlr_xcursor_manager_create(m_xcursorTheme.empty() ? nullptr : m_xcursorTheme.c_str(), m_xcursorSize);

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

    m_touchDown.notify = onTouchDown;
    wl_signal_add(&m_cursor->events.touch_down, &m_touchDown);
    m_touchUp.notify = onTouchUp;
    wl_signal_add(&m_cursor->events.touch_up, &m_touchUp);
    m_touchMotion.notify = onTouchMotion;
    wl_signal_add(&m_cursor->events.touch_motion, &m_touchMotion);
    m_touchCancel.notify = onTouchCancel;
    wl_signal_add(&m_cursor->events.touch_cancel, &m_touchCancel);
    m_touchFrame.notify = onTouchFrame;
    wl_signal_add(&m_cursor->events.touch_frame, &m_touchFrame);

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
    wl_list_remove(&m_touchDown.link);
    wl_list_remove(&m_touchUp.link);
    wl_list_remove(&m_touchMotion.link);
    wl_list_remove(&m_touchCancel.link);
    wl_list_remove(&m_touchFrame.link);
    wlr_cursor_destroy(m_cursor);
    wlr_xcursor_manager_destroy(m_xcursorManager);
  }

  void Cursor::attachInputDevice(wlr_input_device* device) { wlr_cursor_attach_input_device(m_cursor, device); }
  void Cursor::applyConfig() {
    const Config::Input::Cursor& configured = config().input.cursor;
    if (configured.theme == m_xcursorTheme && configured.size == m_xcursorSize) {
      return;
    }

    wlr_xcursor_manager* manager =
        wlr_xcursor_manager_create(configured.theme.empty() ? nullptr : configured.theme.c_str(), configured.size);
    if (manager == nullptr) {
      return;
    }

    wlr_xcursor_manager* oldManager = m_xcursorManager;
    m_xcursorManager = manager;
    m_xcursorTheme = configured.theme;
    m_xcursorSize = configured.size;

    if (m_activeXcursorManager == oldManager) {
      setXcursor(m_activeXcursorName.c_str());
    } else if (m_server->seat()->wlr()->pointer_state.focused_surface == nullptr) {
      setXcursor("default");
    }
    wlr_xcursor_manager_destroy(oldManager);
  }

  void Cursor::setCursorSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY) {
    wlr_cursor_set_surface(m_cursor, surface, hotspotX, hotspotY);
    m_activeXcursorManager = nullptr;
    m_activeXcursorName.clear();
  }

  void Cursor::setXcursor(const char* name) {
    wlr_cursor_set_xcursor(m_cursor, m_xcursorManager, name);
    m_activeXcursorManager = m_xcursorManager;
    m_activeXcursorName = name;
  }

  void Cursor::beginInteractive(View* view, CursorMode mode, uint32_t edges) {
    if (view == nullptr) {
      return;
    }
    if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
        scratchpad != nullptr && scratchpad->contains(view)) {
      view->setFloating(true);
      if (mode == CursorMode::MoveTile) {
        mode = CursorMode::Move;
      } else if (mode == CursorMode::ResizeTile) {
        mode = CursorMode::Resize;
      }
    }
    setActiveConstraint(nullptr);
    m_grabbedView = view;
    m_mode = mode;
    m_resizeEdges = edges;

    if (mode == CursorMode::ResizeTile) {
      m_resizeWorkspace = view->workspace();
      if (m_resizeWorkspace == nullptr
          || m_resizeWorkspace->group() == nullptr
          || m_resizeWorkspace->group()->output() == nullptr) {
        resetMode();
        return;
      }
      Layout& layout = m_resizeWorkspace->layout();
      m_resizeEdges = layout.resolveResizeEdges(view, m_resizeEdges, m_cursor->x, m_cursor->y);
      if (m_resizeEdges == 0) {
        resetMode();
        return;
      }
      const wlr_box usable = m_resizeWorkspace->group()->output()->usableArea();
      m_resizeGrab = layout.beginResize(view, m_resizeEdges, usable);
      if (m_resizeGrab == nullptr) {
        resetMode();
        return;
      }
      m_resizeStartX = m_cursor->x;
      m_resizeStartY = m_cursor->y;
      if (m_resizeGrab->unmaximizeOnBegin()) {
        wlr_xdg_toplevel_set_maximized(view->toplevel(), false);
      }
      updateInteractiveCursor(view);
      return;
    }

    if (mode == CursorMode::Move || mode == CursorMode::MoveTile) {
      m_grabX = m_cursor->x - view->sceneTree()->node.x;
      m_grabY = m_cursor->y - view->sceneTree()->node.y;
      if (mode == CursorMode::Move && !view->pinned()) {
        wlr_scene_node_reparent(&view->sceneTree()->node, m_server->dragTree());
        wlr_scene_node_raise_to_top(&view->sceneTree()->node);
      }
      if (mode == CursorMode::MoveTile) {
        m_tileDragPending = true;
        m_tileDragStartX = m_cursor->x;
        m_tileDragStartY = m_cursor->y;
        m_dragSourceWorkspace = view->workspace();
        m_dragSourceColumn = m_dragSourceWorkspace != nullptr ? m_dragSourceWorkspace->layout().columnOf(view) : -1;
        m_hasDragSourceWidth = false;
        const ScrollingLayout* dragSourceScrolling =
            m_dragSourceWorkspace != nullptr ? m_dragSourceWorkspace->scrollingLayout() : nullptr;
        if (dragSourceScrolling != nullptr && m_dragSourceColumn >= 0) {
          const auto& columns = dragSourceScrolling->columns();
          if (m_dragSourceColumn < static_cast<int>(columns.size())) {
            const Column& column = columns[static_cast<size_t>(m_dragSourceColumn)];
            if (column.views.size() == 1) {
              m_dragSourceWidth = {
                  .fraction = column.savedWidthFrac > 0.0 ? column.savedWidthFrac : column.widthFrac,
                  .fullWidth = column.savedWidthFrac > 0.0,
              };
              m_hasDragSourceWidth = true;
            }
          }
        }
        m_drop = {
            .workspace = m_dragSourceWorkspace,
            .column = std::max(0, m_dragSourceColumn),
        };
      }
      updateInteractiveCursor(view);
      return;
    }

    wlr_box* geo = &view->toplevel()->base->geometry;
    double borderX = (view->sceneTree()->node.x + geo->x) + ((edges & WLR_EDGE_RIGHT) != 0 ? geo->width : 0);
    double borderY = (view->sceneTree()->node.y + geo->y) + ((edges & WLR_EDGE_BOTTOM) != 0 ? geo->height : 0);
    m_grabX = m_cursor->x - borderX;
    m_grabY = m_cursor->y - borderY;
    m_grabGeoX = geo->x + view->sceneTree()->node.x;
    m_grabGeoY = geo->y + view->sceneTree()->node.y;
    m_grabGeoWidth = geo->width;
    m_grabGeoHeight = geo->height;
    view->beginFloatingResize(edges);
    updateInteractiveCursor(view);
  }

  void Cursor::resetMode() {
    m_server->hideInsertHint();
    const CursorMode previousMode = m_mode;
    if ((previousMode == CursorMode::Move || previousMode == CursorMode::MoveTile)
        && m_grabbedView != nullptr
        && m_grabbedView->workspace() != nullptr) {
      if (m_grabbedView->pinned()) {
        m_grabbedView->restorePinnedSceneParent();
      } else {
        wlr_scene_node_reparent(
            &m_grabbedView->sceneTree()->node, m_grabbedView->workspace()->viewLayer(m_grabbedView->tiled())
        );
        m_grabbedView->workspace()->restackFloatingViews();
      }
    }
    if (previousMode == CursorMode::Resize && m_grabbedView != nullptr) {
      m_grabbedView->finishFloatingResize();
    }
    m_mode = CursorMode::Passthrough;
    m_grabbedView = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_hasDragSourceWidth = false;
    m_drop = {};
    m_tileDragPending = false;
    m_resizeWorkspace = nullptr;
    m_resizeEdges = 0;
    m_resizeStartX = 0;
    m_resizeStartY = 0;
    m_resizeGrab.reset();
    refreshInteractiveCursor();
  }

  void Cursor::onMotion(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_motion);
    self->handleMotion(data);
  }

  void Cursor::onMotionAbsolute(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_motionAbsolute);
    self->handleMotionAbsolute(data);
  }

  void Cursor::onButton(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_button);
    self->handleButton(data);
  }

  void Cursor::onAxis(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_axis);
    self->handleAxis(data);
  }

  void Cursor::onFrame(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_frame);
    self->handleFrame();
  }

  void Cursor::handleMotion(void* data) {
    auto* event = static_cast<wlr_pointer_motion_event*>(data);
    m_server->notifyIdleActivity();

    wlr_relative_pointer_manager_v1_send_relative_motion(
        m_server->relativePointerManager(), m_server->seat()->wlr(), static_cast<uint64_t>(event->time_msec) * 1000,
        event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy
    );

    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      // Workspace switch (etc.) can hide the locking surface without pointer motion.
      // Drop the lock so the cursor can move and become visible again.
      if (!constraintSurfaceActive()) {
        clearConstraint();
      } else {
        return;
      }
    }

    double dx = event->delta_x;
    double dy = event->delta_y;
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
      if (!confineDelta(&dx, &dy)) {
        return;
      }
    }

    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
    processMotion(event->time_msec, oldX, oldY);
  }

  void Cursor::handleMotionAbsolute(void* data) {
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    m_server->notifyIdleActivity();
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      if (!constraintSurfaceActive()) {
        clearConstraint();
      } else {
        return;
      }
    }

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->pointer->base, event->x, event->y, &lx, &ly);

    const double oldX = m_cursor->x;
    const double oldY = m_cursor->y;
    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
      double dx = lx - m_cursor->x;
      double dy = ly - m_cursor->y;
      if (!confineDelta(&dx, &dy)) {
        return;
      }
      wlr_cursor_move(m_cursor, &event->pointer->base, dx, dy);
    } else {
      wlr_cursor_warp_absolute(m_cursor, &event->pointer->base, event->x, event->y);
    }
    processMotion(event->time_msec, oldX, oldY);
  }

  void Cursor::handleButton(void* data) {
    auto* event = static_cast<wlr_pointer_button_event*>(data);
    m_server->notifyIdleActivity();

    // Config mouse binds win over the overview and the built-in Mod+drag /
    // Mod+resize grabs. Presses consumed here swallow their paired release so
    // clients never see an unmatched release.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED
        && !m_server->sessionLocked()
        && m_mode == CursorMode::Passthrough) {
      wlr_keyboard* kb = wlr_seat_get_keyboard(m_server->seat()->wlr());
      const uint32_t modifiers = kb != nullptr ? wlr_keyboard_get_modifiers(kb) : 0;
      if (m_server->handleMouseBind(event->button, modifiers)) {
        m_swallowedButtons.push_back(event->button);
        return;
      }
    }
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && std::erase(m_swallowedButtons, event->button) > 0) {
      return;
    }

    // Overview owns the pointer while it is up: cards are its own hit-test
    // surface and the desktop underneath is inert. Top/overlay layer surfaces
    // (panels) stay fully interactive.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked()) {
      const bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      wlr_seat* seat = m_server->seat()->wlr();
      if (overviewPassthroughLayer(layer) && !overview->dragging()) {
        if (surface != nullptr) {
          wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        }
        wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
        if (pressed) {
          layer->focus();
        }
        return;
      }
      overview->handleButton(event->button, pressed, m_cursor->x, m_cursor->y);
      return;
    }

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
      if (m_mode == CursorMode::MoveTile) {
        if (m_tileDragPending) {
          resetMode();
        } else {
          finishTileMove();
        }
        return;
      }
      if (m_mode == CursorMode::Move) {
        finishFloatMove();
        return;
      }
      if (m_mode == CursorMode::ResizeTile) {
        if (m_resizeWorkspace != nullptr) {
          m_resizeWorkspace->arrange(false);
        }
        resetMode();
        return;
      }
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), event->time_msec, event->button, event->state);

      // After the final release, refresh pointer focus so it matches the
      // surface actually under the cursor.  The implicit-grab guard in
      // processMotion kept focus pinned while buttons were held; realign
      // now so a subsequent press without intervening motion targets the
      // correct surface.
      if (m_server->seat()->wlr()->pointer_state.button_count == 0) {
        double sx2 = 0;
        double sy2 = 0;
        wlr_surface* surf = nullptr;
        m_server->viewAt(m_cursor->x, m_cursor->y, &surf, &sx2, &sy2);
        if (surf != nullptr) {
          wlr_seat_pointer_notify_enter(m_server->seat()->wlr(), surf, sx2, sy2);
        } else {
          wlr_seat_pointer_clear_focus(m_server->seat()->wlr());
        }
      }

      resetMode();
      return;
    }

    // An implicit grab belongs to the surface that received the first press.
    // Route additional presses there until every button has been released.
    if (wlr_seat* seat = m_server->seat()->wlr(); seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr) {
      wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    if (m_server->sessionLocked()) {
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), event->time_msec, event->button, event->state);
      if (surface != nullptr) {
        if (wlr_session_lock_surface_v1* lockSurface = wlr_session_lock_surface_v1_try_from_wlr_surface(surface)) {
          if (auto* node = static_cast<LockSurface*>(lockSurface->data)) {
            node->focus();
          }
        }
      }
      return;
    }

    wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_server->seat()->wlr());
    const bool modHeld = keyboard != nullptr && (wlr_keyboard_get_modifiers(keyboard) & m_server->modKey()) != 0;
    if (event->button == BTN_LEFT && modHeld && view != nullptr) {
      m_server->focusView(view, FocusReason::Grab);
      beginInteractive(view, view->tiled() ? CursorMode::MoveTile : CursorMode::Move, 0);
      return;
    }
    if (event->button == BTN_RIGHT && modHeld && view != nullptr) {
      m_server->focusView(view, FocusReason::Grab);
      if (view->tiled()) {
        beginInteractive(view, CursorMode::ResizeTile, 0);
      } else {
        beginInteractive(view, CursorMode::Resize, floatResizeEdges(view));
      }
      return;
    }

    // Pointer focus must match the surface under the cursor before the button
    // event so wl_data_device drag serial validation succeeds.
    wlr_seat* seat = m_server->seat()->wlr();
    if (surface != nullptr) {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    } else {
      wlr_seat_pointer_clear_focus(seat);
    }

    wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
    if (layer != nullptr) {
      layer->focus();
    } else if (m_server->exclusiveKeyboardLayer() == nullptr) {
      if (view != nullptr) {
        m_server->focusView(view, FocusReason::PointerPress);
      } else {
        wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
        m_server->refocus(m_server->outputFromWlr(wlrOutput));
      }
    }
  }

  void Cursor::handleAxis(void* data) {
    auto* event = static_cast<wlr_pointer_axis_event*>(data);
    m_server->notifyIdleActivity();

    wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_server->seat()->wlr());
    const uint32_t modifiers = keyboard != nullptr ? wlr_keyboard_get_modifiers(keyboard) : 0;
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);

    // Determine this event's signed wheel direction from delta and orientation.
    const bool isVertical = event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
    const double rawDelta = event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) : event->delta;
    WheelDirection eventDir;
    if (isVertical) {
      eventDir = rawDelta < 0 ? WheelDirection::Up : WheelDirection::Down;
    } else {
      eventDir = rawDelta < 0 ? WheelDirection::Left : WheelDirection::Right;
    }

    // Unmodified scrolling drives the overview filmstrip instead of the inert
    // desktop under the cursor. Panels (top/overlay) keep their own scrolling,
    // and modifier chords still fall through to the wheel binds below.
    if (Overview* overview = m_server->overview(); overview != nullptr && overview->active() && effective == 0) {
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (!overviewPassthroughLayer(layer)) {
        if (!overview->interactive()) {
          return;
        }
        const int axis = isVertical ? 0 : 1;
        m_wheelAccum[axis] +=
            event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0;
        double& accumulated = m_wheelAccum[axis];
        while (std::abs(accumulated) >= 1.0) {
          overview->handleAxisNotch(isVertical, accumulated, m_cursor->x, m_cursor->y);
          accumulated -= std::copysign(1.0, accumulated);
        }
        return;
      }
    }

    // Arm only when a bind matches this exact direction and modifier set.
    bool armed = false;
    for (const Keybind& bind : config().keybinds) {
      if (bind.wheel != eventDir) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? m_server->modKey() : 0);
      if (effective == expected) {
        armed = true;
        break;
      }
    }

    const int orientation = isVertical ? 0 : 1;
    if (!armed) {
      m_wheelAccum[orientation] = 0;
      wlr_seat_pointer_notify_axis(
          m_server->seat()->wlr(), event->time_msec, event->orientation, event->delta, event->delta_discrete,
          event->source, event->relative_direction
      );
      return;
    }

    // Accumulate normalized notches.
    const double notches =
        event->delta_discrete != 0 ? static_cast<double>(event->delta_discrete) / 120.0 : event->delta / 15.0;
    m_wheelAccum[orientation] += notches;

    double& acc = m_wheelAccum[orientation];
    while (std::abs(acc) >= 1.0) {
      WheelDirection direction;
      if (isVertical) {
        direction = acc < 0 ? WheelDirection::Up : WheelDirection::Down;
      } else {
        direction = acc < 0 ? WheelDirection::Left : WheelDirection::Right;
      }
      m_server->handleWheelBind(direction, modifiers);
      acc -= std::copysign(1.0, acc);
    }
  }

  void Cursor::handleFrame() { wlr_seat_pointer_notify_frame(m_server->seat()->wlr()); }

  void Cursor::onTouchDown(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchDown);
    self->handleTouchDown(data);
  }

  void Cursor::onTouchUp(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchUp);
    self->handleTouchUp(data);
  }

  void Cursor::onTouchMotion(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchMotion);
    self->handleTouchMotion(data);
  }

  void Cursor::onTouchCancel(wl_listener* listener, void* data) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchCancel);
    self->handleTouchCancel(data);
  }

  void Cursor::onTouchFrame(wl_listener* listener, void* /*data*/) {
    Cursor* self;
    self = wl_container_of(listener, self, m_touchFrame);
    self->handleTouchFrame();
  }

  void Cursor::handleTouchDown(void* data) {
    auto* event = static_cast<wlr_touch_down_event*>(data);
    m_server->notifyIdleActivity();

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->touch->base, event->x, event->y, &lx, &ly);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(lx, ly, &surface, &sx, &sy, &layer);

    // A tap in overview activates like a left click; panels keep their touch.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked() && !overviewPassthroughLayer(layer)) {
      if (overview->interactive()) {
        overview->handleButton(BTN_LEFT, true, lx, ly);
        overview->handleButton(BTN_LEFT, false, lx, ly);
      }
      return;
    }

    if (surface != nullptr) {
      // Focus the touched view (click-to-focus equivalent).
      if (!m_server->sessionLocked() && m_server->exclusiveKeyboardLayer() == nullptr) {
        if (layer != nullptr) {
          layer->focus();
        } else if (view != nullptr) {
          m_server->focusView(view, FocusReason::PointerPress);
        }
      }
      wlr_seat_touch_notify_down(m_server->seat()->wlr(), surface, event->time_msec, event->touch_id, sx, sy);
    }
  }

  void Cursor::handleTouchUp(void* data) {
    auto* event = static_cast<wlr_touch_up_event*>(data);
    m_server->notifyIdleActivity();
    wlr_seat_touch_notify_up(m_server->seat()->wlr(), event->time_msec, event->touch_id);
  }

  void Cursor::handleTouchMotion(void* data) {
    auto* event = static_cast<wlr_touch_motion_event*>(data);
    m_server->notifyIdleActivity();

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_touch_point* point = wlr_seat_touch_get_point(seat, event->touch_id);
    if (point == nullptr || point->focus_surface == nullptr) {
      return;
    }

    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(m_cursor, &event->touch->base, event->x, event->y, &lx, &ly);

    double sx = 0;
    double sy = 0;
    if (!surfaceLocalCoordinates(m_server->scene(), point->focus_surface, lx, ly, &sx, &sy)) {
      return;
    }
    wlr_seat_touch_notify_motion(seat, event->time_msec, event->touch_id, sx, sy);
  }

  void Cursor::handleTouchCancel(void* data) {
    auto* event = static_cast<wlr_touch_cancel_event*>(data);
    (void)event;
    m_server->notifyIdleActivity();

    wlr_seat* seat = m_server->seat()->wlr();
    // Find the first client with an active touch point, then cancel outside
    // the iteration — wlr_seat_touch_notify_cancel may mutate the list.
    wlr_seat_client* client = nullptr;
    wlr_touch_point* point;
    wl_list_for_each(point, &seat->touch_state.touch_points, link) {
      if (point->client != nullptr) {
        client = point->client;
        break;
      }
    }
    if (client != nullptr) {
      wlr_seat_touch_notify_cancel(seat, client);
    }
  }

  void Cursor::handleTouchFrame() { wlr_seat_touch_notify_frame(m_server->seat()->wlr()); }

  void Cursor::processMotion(uint32_t timeMsec, double oldX, double oldY) {
    // Overview owns motion: cards follow a drag, panels keep passthrough, and
    // the inert desktop underneath never receives enter/motion or hover focus.
    if (Overview* overview = m_server->overview();
        overview != nullptr && overview->active() && !m_server->sessionLocked()) {
      overview->handleMotion(m_cursor->x, m_cursor->y);
      wlr_seat* seat = m_server->seat()->wlr();
      if (overview->dragging()) {
        wlr_seat_pointer_clear_focus(seat);
        return;
      }
      double sx = 0;
      double sy = 0;
      wlr_surface* surface = nullptr;
      LayerSurface* layer = nullptr;
      m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      if (overviewPassthroughLayer(layer) && surface != nullptr) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
        return;
      }
      wlr_seat_pointer_clear_focus(seat);
      if (!m_compositorOwnsCursor) {
        setXcursor("default");
      }
      return;
    }

    if (m_mode == CursorMode::Move || m_mode == CursorMode::MoveTile) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        if (m_mode == CursorMode::MoveTile && m_tileDragPending) {
          constexpr double kDragThreshold = 10.0;
          const double dx = m_cursor->x - m_tileDragStartX;
          const double dy = m_cursor->y - m_tileDragStartY;
          if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
            return;
          }
          m_tileDragPending = false;
          m_grabbedView->cancelPositionAnimation();
          if (m_dragSourceWorkspace != nullptr) {
            m_dragSourceWorkspace->layoutDetach(m_grabbedView);
          }
          // Present a dragged tile above every workspace window while it moves.
          wlr_scene_node_reparent(&m_grabbedView->sceneTree()->node, m_server->dragTree());
          wlr_scene_node_raise_to_top(&m_grabbedView->sceneTree()->node);
          presentGrabbedViewSpanning();
        }
        processMove();
        if (m_mode == CursorMode::MoveTile) {
          updateDropTarget();
        }
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
    if (m_mode == CursorMode::ResizeTile) {
      if (m_server->sessionLocked()) {
        resetMode();
      } else {
        processResizeTile();
        return;
      }
    }

    wlr_seat* seat = m_server->seat()->wlr();
    if (seat->drag == nullptr
        && seat->pointer_state.button_count > 0
        && seat->pointer_state.focused_surface != nullptr) {
      // Keep an implicit grab in the coordinate space established by the
      // press. Re-resolving against the scene would turn compositor-driven
      // window animation into apparent pointer travel and make small clicks
      // look like client drags.
      const double sx = seat->pointer_state.sx + (m_cursor->x - oldX);
      const double sy = seat->pointer_state.sy + (m_cursor->y - oldY);
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
      updateConstraintForSurface(seat->pointer_state.focused_surface);
      return;
    }
    // Crossing outputs updates keyboard / foreign-toplevel focus so clients that follow the
    // focused screen match preferredOutput() / workspace-switch behavior.
    wlr_output* pointerOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    if (pointerOutput != m_pointerOutput) {
      m_pointerOutput = pointerOutput;
      if (config().input.focus.followsMouse
          && !m_server->sessionLocked()
          && m_server->exclusiveKeyboardLayer() == nullptr) {
        m_server->refocus(m_server->outputFromWlr(pointerOutput));
      }
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    if (config().input.focus.followsMouse
        && seat->drag == nullptr
        && !m_server->sessionLocked()
        && layer == nullptr
        && view != nullptr
        && view->mapped()) {
      // Only activate when the pointer enters a different window (under old pos
      // != under new pos). Do not warp the pointer with scroll — that re-arms enters
      // during a swipe and cascades across columns.
      wlr_surface* oldSurface = nullptr;
      double oldSx = 0;
      double oldSy = 0;
      View* oldView = m_server->viewAt(oldX, oldY, &oldSurface, &oldSx, &oldSy);
      const bool entered = view != oldView;
      const bool alreadyFocused = view->workspace() != nullptr && view->workspace()->focusedView() == view;
      if (entered && !alreadyFocused) {
        m_server->focusView(view, FocusReason::PointerHover);
        // Scroll may have moved another surface under the cursor; refresh hit-test for
        // pointer notify only. Keyboard focus stays on the entered view until a real enter.
        view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
      }
    }

    if (surface != nullptr) {
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
      wlr_seat_pointer_notify_motion(seat, timeMsec, sx, sy);
    } else if (!m_compositorOwnsCursor) {
      setXcursor("default");
      wlr_seat_pointer_clear_focus(seat);
    } else {
      wlr_seat_pointer_clear_focus(seat);
    }

    // Update the drag icon after seat motion so drop targets are recognized.
    if (seat->drag != nullptr && seat->drag->icon != nullptr) {
      wlr_scene_node_set_position(
          &m_server->dragIconTree()->node, static_cast<int>(m_cursor->x), static_cast<int>(m_cursor->y)
      );
    }

    updateConstraintForSurface(surface);
    updateInteractiveCursor(view);
  }

  void Cursor::processMove() {
    const int nx = static_cast<int>(m_cursor->x - m_grabX);
    const int ny = static_cast<int>(m_cursor->y - m_grabY);
    m_grabbedView->setDragPosition(nx, ny);
    // Both floating (Move) and tiled (MoveTile) drags keep the view unclipped so
    // it spans outputs as the pointer crosses monitors.
    presentGrabbedViewSpanning();
  }

  void Cursor::presentGrabbedViewSpanning() {
    if (m_grabbedView == nullptr) {
      return;
    }
    // A window dragged across a monitor boundary must span both outputs, not be
    // clipped to one (which flickers A<->B as the pointer crosses). Leave it
    // unclipped and enabled; native per-output rendering draws each half. This
    // is the accepted trade for dropping per-output clipping.
    m_grabbedView->setNodeEnabled(true);
    m_grabbedView->clearOutputClip();
  }

  void Cursor::updateDropTarget() {
    if (m_mode != CursorMode::MoveTile || m_grabbedView == nullptr) {
      return;
    }
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (output == nullptr || output->workspaceGroup() == nullptr || output->workspaceGroup()->active() == nullptr) {
      return;
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
    if (layer != nullptr) {
      return;
    }

    Workspace* workspace = output->workspaceGroup()->active();
    m_drop = computeDropTarget(*workspace, m_cursor->x, m_cursor->y, m_grabbedView);
    if (m_drop.hintBox.width > 0 && m_drop.hintBox.height > 0) {
      m_server->insertHint().show(output, m_drop.hintBox, config().appearance.cornerRadius);
    } else {
      m_server->hideInsertHint();
    }
    wlr_scene_node_raise_to_top(&m_grabbedView->sceneTree()->node);
  }

  void Cursor::finishTileMove() {
    m_server->hideInsertHint();
    View* view = m_grabbedView;
    Workspace* target = m_drop.workspace != nullptr ? m_drop.workspace : m_dragSourceWorkspace;
    if (view != nullptr && view->mapped() && target != nullptr) {
      const bool restoreSourceWidth = m_hasDragSourceWidth
          && target == m_dragSourceWorkspace
          && m_drop.row < 0
          && std::max(0, m_drop.column) == m_dragSourceColumn;
      applyDrop(*m_server, *view, *target, m_drop, restoreSourceWidth ? &m_dragSourceWidth : nullptr, /*animate=*/true);
    }
    resetMode();
  }

  void Cursor::finishFloatMove() {
    View* view = m_grabbedView;
    if (view != nullptr && view->mapped()) {
      const int x = view->sceneTree()->node.x;
      const int y = view->sceneTree()->node.y;
      wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
      Output* output = m_server->outputFromWlr(wlrOutput);
      if (ScratchpadManager* scratchpad = m_server->scratchpadManager();
          scratchpad != nullptr && scratchpad->contains(view)) {
        scratchpad->finishMove(view, output);
        view->setPosition(x, y);
        m_server->focusView(view, FocusReason::DragDrop);
        resetMode();
        return;
      }
      // Own the drop output so per-frame home-output culling does not hide the window.
      if (output != nullptr && output->workspaceGroup() != nullptr) {
        if (Workspace* target = output->workspaceGroup()->active()) {
          if (view->workspace() != target) {
            view->setWorkspace(target);
            view->setPosition(x, y);
          }
        }
      }
      view->setNodeEnabled(true);
      if (view->pinned()) {
        view->restorePinnedSceneParent();
      } else if (view->workspace() != nullptr) {
        wlr_scene_node_reparent(&view->sceneTree()->node, view->workspace()->viewLayer(false));
      }
      if (view->workspace() != nullptr) {
        view->workspace()->syncViewPresentation(view);
      }
      m_server->focusView(view, FocusReason::DragDrop);
    }
    resetMode();
  }

  void Cursor::processResize() {
    double borderX = m_cursor->x - m_grabX;
    double borderY = m_cursor->y - m_grabY;
    int newLeft = m_grabGeoX;
    int newRight = m_grabGeoX + m_grabGeoWidth;
    int newTop = m_grabGeoY;
    int newBottom = m_grabGeoY + m_grabGeoHeight;
    const XdgSizeHints hints = xdgSizeHints(m_grabbedView->toplevel());

    if ((m_resizeEdges & WLR_EDGE_TOP) != 0) {
      newTop = static_cast<int>(borderY);
      if (newBottom - newTop < hints.minHeight) {
        newTop = newBottom - hints.minHeight;
      }
      if (hints.maxHeight > 0 && newBottom - newTop > hints.maxHeight) {
        newTop = newBottom - hints.maxHeight;
      }
    } else if ((m_resizeEdges & WLR_EDGE_BOTTOM) != 0) {
      newBottom = static_cast<int>(borderY);
      if (newBottom - newTop < hints.minHeight) {
        newBottom = newTop + hints.minHeight;
      }
      if (hints.maxHeight > 0 && newBottom - newTop > hints.maxHeight) {
        newBottom = newTop + hints.maxHeight;
      }
    }

    if ((m_resizeEdges & WLR_EDGE_LEFT) != 0) {
      newLeft = static_cast<int>(borderX);
      if (newRight - newLeft < hints.minWidth) {
        newLeft = newRight - hints.minWidth;
      }
      if (hints.maxWidth > 0 && newRight - newLeft > hints.maxWidth) {
        newLeft = newRight - hints.maxWidth;
      }
    } else if ((m_resizeEdges & WLR_EDGE_RIGHT) != 0) {
      newRight = static_cast<int>(borderX);
      if (newRight - newLeft < hints.minWidth) {
        newRight = newLeft + hints.minWidth;
      }
      if (hints.maxWidth > 0 && newRight - newLeft > hints.maxWidth) {
        newRight = newLeft + hints.maxWidth;
      }
    }

    m_grabbedView->resizeFloating(newRight - newLeft, newBottom - newTop);
  }

  uint32_t Cursor::floatResizeEdges(View* view) const {
    if (view == nullptr || view->sceneTree() == nullptr) {
      return WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
    }
    const wlr_box& geo = view->toplevel()->base->geometry;
    const int x = view->sceneTree()->node.x + geo.x;
    const int y = view->sceneTree()->node.y + geo.y;
    const double cx = m_cursor->x;
    const double cy = m_cursor->y;
    const double distLeft = std::abs(cx - x);
    const double distRight = std::abs(cx - (x + geo.width));
    const double distTop = std::abs(cy - y);
    const double distBottom = std::abs(cy - (y + geo.height));
    const double nearestH = std::min(distLeft, distRight);
    const double nearestV = std::min(distTop, distBottom);

    uint32_t edges = 0;
    if (nearestH <= nearestV) {
      edges |= distLeft <= distRight ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
    } else {
      edges |= distTop <= distBottom ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
    }
    // Prefer a corner when the cursor is near both axes.
    constexpr double kCornerSlop = 32.0;
    if (nearestH < kCornerSlop && nearestV < kCornerSlop) {
      edges = (distLeft <= distRight ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT)
          | (distTop <= distBottom ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
    }
    return edges;
  }

  uint32_t Cursor::hoverResizeEdges(View* view) const {
    if (view == nullptr) {
      return 0;
    }
    // Only advertise resize when the pointer is near the edge that would be grabbed.
    constexpr double kHoverSlop = 28.0;

    if (view->floating()) {
      if (view->sceneTree() == nullptr) {
        return 0;
      }
      const wlr_box& geo = view->toplevel()->base->geometry;
      const double left = view->sceneTree()->node.x + geo.x;
      const double top = view->sceneTree()->node.y + geo.y;
      const double right = left + geo.width;
      const double bottom = top + geo.height;
      const double distLeft = std::abs(m_cursor->x - left);
      const double distRight = std::abs(m_cursor->x - right);
      const double distTop = std::abs(m_cursor->y - top);
      const double distBottom = std::abs(m_cursor->y - bottom);
      const double nearestH = std::min(distLeft, distRight);
      const double nearestV = std::min(distTop, distBottom);
      if (std::min(nearestH, nearestV) > kHoverSlop) {
        return 0;
      }
      return floatResizeEdges(view);
    }

    if (view->workspace() == nullptr) {
      return 0;
    }
    const wlr_box box = view->workspace()->layout().targetBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return 0;
    }
    const uint32_t edges = view->workspace()->layout().resizeEdgesAt(view, m_cursor->x, m_cursor->y);
    double dist = kHoverSlop + 1.0;
    if ((edges & WLR_EDGE_LEFT) != 0) {
      dist = std::abs(m_cursor->x - box.x);
    } else if ((edges & WLR_EDGE_RIGHT) != 0) {
      dist = std::abs(m_cursor->x - (box.x + box.width));
    } else if ((edges & WLR_EDGE_TOP) != 0) {
      dist = std::abs(m_cursor->y - box.y);
    } else if ((edges & WLR_EDGE_BOTTOM) != 0) {
      dist = std::abs(m_cursor->y - (box.y + box.height));
    }
    return dist <= kHoverSlop ? edges : 0;
  }

  void Cursor::setCompositorCursor(const char* name) {
    if (name == nullptr) {
      if (m_compositorOwnsCursor) {
        restoreClientCursor();
      }
      return;
    }
    if (m_compositorOwnsCursor && m_compositorCursorName == name) {
      return;
    }
    m_compositorOwnsCursor = true;
    m_compositorCursorName = name;
    setXcursor(name);
  }

  void Cursor::restoreClientCursor() {
    m_compositorOwnsCursor = false;
    m_compositorCursorName.clear();

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    wlr_seat* seat = m_server->seat()->wlr();
    if (surface != nullptr) {
      // Re-enter so the client can restore its pointer shape.
      wlr_seat_pointer_clear_focus(seat);
      wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    } else {
      setXcursor("default");
      wlr_seat_pointer_clear_focus(seat);
    }
  }

  void Cursor::updateInteractiveCursor(View* under) {
    if (m_server->sessionLocked()) {
      setCompositorCursor(nullptr);
      return;
    }

    if (m_mode == CursorMode::Resize || m_mode == CursorMode::ResizeTile) {
      const char* name = wlr_xcursor_get_resize_name(static_cast<enum wlr_edges>(m_resizeEdges));
      setCompositorCursor(name != nullptr ? name : "default");
      return;
    }
    if (m_mode == CursorMode::Move || m_mode == CursorMode::MoveTile) {
      setCompositorCursor("grabbing");
      return;
    }

    wlr_keyboard* keyboard = wlr_seat_get_keyboard(m_server->seat()->wlr());
    const bool modHeld = keyboard != nullptr && (wlr_keyboard_get_modifiers(keyboard) & m_server->modKey()) != 0;
    if (modHeld && under != nullptr && under->mapped()) {
      const uint32_t edges = hoverResizeEdges(under);
      if (edges != 0) {
        const char* name = wlr_xcursor_get_resize_name(static_cast<enum wlr_edges>(edges));
        setCompositorCursor(name != nullptr ? name : "default");
        return;
      }
      setCompositorCursor("grab");
      return;
    }

    setCompositorCursor(nullptr);
  }

  void Cursor::refreshInteractiveCursor() {
    if (m_mode != CursorMode::Passthrough) {
      updateInteractiveCursor(m_grabbedView);
      return;
    }
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy);
    updateInteractiveCursor(view);
  }

  void Cursor::processResizeTile() {
    if (m_resizeWorkspace == nullptr
        || m_resizeWorkspace->group() == nullptr
        || m_resizeWorkspace->group()->output() == nullptr
        || m_grabbedView == nullptr
        || m_resizeGrab == nullptr) {
      resetMode();
      return;
    }
    // A config reload can swap the workspace's layout mid-grab, dangling the
    // session's cached layout pointer. Bail if the layout is no longer the one
    // the session was created against.
    if (m_resizeGrab->ownerLayout() != &m_resizeWorkspace->layout()) {
      resetMode();
      return;
    }
    const wlr_box usable = m_resizeWorkspace->group()->output()->usableArea();
    m_resizeGrab->applyDelta(m_cursor->x - m_resizeStartX, m_cursor->y - m_resizeStartY, usable);
    wlr_xdg_toplevel_set_maximized(m_grabbedView->toplevel(), false);
    m_resizeWorkspace->arrange(false);
  }

} // namespace umbriel
