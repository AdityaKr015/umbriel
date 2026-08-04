#include "input/cursor.h"

#include "config/config.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "layout/insert_hint.h"
#include "layout/scrolling.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  Cursor::Cursor(Server& server) : m_server(&server) {
    m_cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(m_cursor, m_server->outputLayout());
    const Config::Input::Cursor& configured = config().input.cursor;
    m_xcursorManager =
        wlr_xcursor_manager_create(configured.theme.empty() ? nullptr : configured.theme.c_str(), configured.size);

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

  void Cursor::attachInputDevice(wlr_input_device* device) { wlr_cursor_attach_input_device(m_cursor, device); }
  void Cursor::applyConfig() {
    const Config::Input::Cursor& configured = config().input.cursor;
    wlr_xcursor_manager* manager =
        wlr_xcursor_manager_create(configured.theme.empty() ? nullptr : configured.theme.c_str(), configured.size);
    if (manager == nullptr) {
      return;
    }
    wlr_xcursor_manager_destroy(m_xcursorManager);
    m_xcursorManager = manager;
    if (m_server->seat()->wlr()->pointer_state.focused_surface == nullptr) {
      wlr_cursor_set_xcursor(m_cursor, m_xcursorManager, "default");
    }
  }

  void Cursor::beginInteractive(View* view, CursorMode mode, uint32_t edges) {
    if (view == nullptr) {
      return;
    }
    setActiveConstraint(nullptr);
    m_grabbedView = view;
    m_mode = mode;
    m_resizeEdges = edges;

    if (mode == CursorMode::ResizeTile) {
      m_resizeWorkspace = view->workspace();
      m_resizeColumn = m_resizeWorkspace != nullptr ? m_resizeWorkspace->layout().columnOf(view) : -1;
      m_resizeRow = m_resizeWorkspace != nullptr ? m_resizeWorkspace->layout().rowOf(view) : -1;
      if (m_resizeEdges == 0) {
        m_resizeEdges = tileResizeEdges(view);
      }
      // Solo column: no vertical resize (gap weights leave a weird state when another window joins).
      if (m_resizeWorkspace != nullptr && m_resizeColumn >= 0) {
        const auto& columns = m_resizeWorkspace->layout().columns();
        if (m_resizeColumn < static_cast<int>(columns.size())
            && columns[static_cast<size_t>(m_resizeColumn)].views.size() < 2) {
          m_resizeEdges &= ~(WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
          if (m_resizeEdges == 0) {
            m_resizeEdges = tileResizeEdges(view);
          }
        }
      }

      m_resizeSoloHorizontal = false;
      if (m_resizeWorkspace != nullptr
          && m_resizeColumn >= 0
          && m_resizeWorkspace->layout().isFullWidth(m_resizeColumn)) {
        m_resizeWorkspace->layout().clearFullWidthState(m_resizeColumn);
        wlr_xdg_toplevel_set_maximized(view->toplevel(), false);
        m_resizeSoloHorizontal = true;
      }

      m_resizeStartX = m_cursor->x;
      m_resizeStartY = m_cursor->y;
      m_resizeStartScroll = m_resizeWorkspace != nullptr ? m_resizeWorkspace->layout().scroll() : 0;
      m_resizeUpperRow = -1;
      m_resizeStartPrevWidthPx = 0;
      m_resizeStartUpperWeight = 0;
      m_resizeStartLowerWeight = 0;

      if (m_resizeWorkspace != nullptr
          && m_resizeWorkspace->group() != nullptr
          && m_resizeWorkspace->group()->output() != nullptr
          && m_resizeColumn >= 0) {
        const wlr_box box = m_resizeWorkspace->layout().targetBox(view);
        m_resizeStartLeft = box.x;
        m_resizeStartRight = box.x + box.width;
        m_resizeStartTop = box.y;
        m_resizeStartBottom = box.y + box.height;
        m_resizeStartWidthPx = box.width;

        const int viewportWidth =
            std::max(1, m_resizeWorkspace->group()->output()->usableArea().width - 2 * config().layoutEdgePad());
        if (m_resizeStartWidthPx >= viewportWidth) {
          m_resizeSoloHorizontal = true;
        }
        if (m_resizeColumn > 0 && !m_resizeSoloHorizontal) {
          m_resizeStartPrevWidthPx = m_resizeWorkspace->layout().columnWidth(m_resizeColumn - 1, viewportWidth);
        }

        if ((m_resizeEdges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0 && m_resizeRow >= 0) {
          const Column& column = m_resizeWorkspace->layout().columns()[static_cast<size_t>(m_resizeColumn)];
          if ((m_resizeEdges & WLR_EDGE_TOP) != 0) {
            if (m_resizeRow > 0) {
              m_resizeUpperRow = m_resizeRow - 1;
              m_resizeStartUpperWeight = m_resizeWorkspace->layout().heightWeight(m_resizeColumn, m_resizeUpperRow);
              m_resizeStartLowerWeight = m_resizeWorkspace->layout().heightWeight(m_resizeColumn, m_resizeRow);
            } else {
              m_resizeUpperRow = -1;
              m_resizeStartUpperWeight = m_resizeWorkspace->layout().topGapWeight(m_resizeColumn);
              m_resizeStartLowerWeight = m_resizeWorkspace->layout().heightWeight(m_resizeColumn, 0);
            }
          } else if ((m_resizeEdges & WLR_EDGE_BOTTOM) != 0) {
            if (m_resizeRow + 1 < static_cast<int>(column.views.size())) {
              m_resizeUpperRow = m_resizeRow;
              m_resizeStartUpperWeight = m_resizeWorkspace->layout().heightWeight(m_resizeColumn, m_resizeRow);
              m_resizeStartLowerWeight = m_resizeWorkspace->layout().heightWeight(m_resizeColumn, m_resizeRow + 1);
            } else {
              m_resizeUpperRow = static_cast<int>(column.views.size()) - 1;
              m_resizeStartUpperWeight = m_resizeWorkspace->layout().heightWeight(m_resizeColumn, m_resizeRow);
              m_resizeStartLowerWeight = m_resizeWorkspace->layout().bottomGapWeight(m_resizeColumn);
            }
          }
        }
      } else {
        m_resizeStartLeft = 0;
        m_resizeStartRight = 0;
        m_resizeStartTop = 0;
        m_resizeStartBottom = 0;
        m_resizeStartWidthPx = 0;
      }
      return;
    }

    if (mode == CursorMode::Move || mode == CursorMode::MoveTile) {
      m_grabX = m_cursor->x - view->sceneTree()->node.x;
      m_grabY = m_cursor->y - view->sceneTree()->node.y;
      if (mode == CursorMode::MoveTile) {
        m_tileDragPending = true;
        m_tileDragStartX = m_cursor->x;
        m_tileDragStartY = m_cursor->y;
        m_dragSourceWorkspace = view->workspace();
        m_dragSourceColumn = m_dragSourceWorkspace != nullptr ? m_dragSourceWorkspace->layout().columnOf(view) : -1;
        m_dropWorkspace = m_dragSourceWorkspace;
        m_dropColumn = std::max(0, m_dragSourceColumn);
        m_dropRow = -1;
      }
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
    if (m_dropWorkspace != nullptr && m_dropWorkspace->layout().insertGap() >= 0) {
      m_dropWorkspace->layout().clearInsertGap();
      m_dropWorkspace->arrange(false);
    }
    m_server->hideInsertHint();
    m_mode = CursorMode::Passthrough;
    m_grabbedView = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_dropWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dropColumn = -1;
    m_dropRow = -1;
    m_tileDragPending = false;
    m_resizeWorkspace = nullptr;
    m_resizeColumn = -1;
    m_resizeRow = -1;
    m_resizeStartX = 0;
    m_resizeStartY = 0;
    m_resizeStartScroll = 0;
    m_resizeStartLeft = 0;
    m_resizeStartRight = 0;
    m_resizeStartWidthPx = 0;
    m_resizeStartPrevWidthPx = 0;
    m_resizeStartTop = 0;
    m_resizeStartBottom = 0;
    m_resizeStartUpperWeight = 0;
    m_resizeStartLowerWeight = 0;
    m_resizeUpperRow = -1;
    m_resizeSoloHorizontal = false;
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
        m_server->relativePointerManager(), m_server->seat()->wlr(), static_cast<uint64_t>(event->time_msec) * 1000,
        event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy
    );

    if (m_activeConstraint != nullptr && m_activeConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
      return;
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
      return;
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
          m_resizeWorkspace->ensureFocusedVisible();
          m_resizeWorkspace->arrange(false);
        }
        resetMode();
        return;
      }
      wlr_seat_pointer_notify_button(m_server->seat()->wlr(), event->time_msec, event->button, event->state);
      resetMode();
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
      m_server->focusView(view);
      beginInteractive(view, view->tiled() ? CursorMode::MoveTile : CursorMode::Move, 0);
      return;
    }
    if (event->button == BTN_RIGHT && modHeld && view != nullptr) {
      m_server->focusView(view);
      if (view->tiled()) {
        beginInteractive(view, CursorMode::ResizeTile, 0);
      } else {
        beginInteractive(view, CursorMode::Resize, floatResizeEdges(view));
      }
      return;
    }

    wlr_seat_pointer_notify_button(m_server->seat()->wlr(), event->time_msec, event->button, event->state);
    if (layer != nullptr) {
      layer->focus();
    } else if (m_server->exclusiveKeyboardLayer() == nullptr) {
      // Do not ensureVisible/scroll on click. Peek adjustment would move the
      // surface under the cursor and break link clicks and in-window drags.
      if (view != nullptr) {
        m_server->focusView(view, false);
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

  void Cursor::processMotion(uint32_t timeMsec, double oldX, double oldY) {
    // DnD icons are parented under dragIconTree; keep that tree on the cursor (dwl-style).
    if (wlr_seat* seat = m_server->seat()->wlr(); seat->drag != nullptr && seat->drag->icon != nullptr) {
      wlr_scene_node_set_position(
          &m_server->dragIconTree()->node, static_cast<int>(m_cursor->x), static_cast<int>(m_cursor->y)
      );
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
          wlr_scene_node_raise_to_top(&m_grabbedView->sceneTree()->node);
          clipGrabbedViewToOutput();
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

    // Crossing outputs updates keyboard / foreign-toplevel focus so clients that follow the
    // focused screen match preferredOutput() / workspace-switch behavior.
    wlr_output* pointerOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    if (pointerOutput != m_pointerOutput) {
      m_pointerOutput = pointerOutput;
      if (!m_server->sessionLocked() && m_server->exclusiveKeyboardLayer() == nullptr) {
        m_server->refocus(m_server->outputFromWlr(pointerOutput));
      }
    }

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    LayerSurface* layer = nullptr;
    View* view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);

    // Implicit pointer grab: while any button is held, keep focus on the
    // surface that received the press so it receives the matching release.
    // Without this, crossing a border/gap/other surface clears or retargets
    // focus and the release is lost — the client never sees button-up.
    // The next motion after all buttons are up refreshes focus naturally.
    wlr_seat* seat = m_server->seat()->wlr();
    if (seat->pointer_state.button_count > 0 && surface != seat->pointer_state.focused_surface) {
      updateConstraintForSurface(seat->pointer_state.focused_surface);
      return;
    }

    if (config().input.focus.followsMouse
        && !m_server->sessionLocked()
        && layer == nullptr
        && view != nullptr
        && view->mapped()) {
      // niri: only activate when the pointer enters a different window (under old pos
      // != under new pos). Do not warp the pointer with scroll — that re-arms enters
      // during a swipe and cascades across columns (e.g. 6 → 5 → … → 1).
      wlr_surface* oldSurface = nullptr;
      double oldSx = 0;
      double oldSy = 0;
      View* oldView = m_server->viewAt(oldX, oldY, &oldSurface, &oldSx, &oldSy);
      const bool entered = view != oldView;
      const bool alreadyFocused = view->workspace() != nullptr && view->workspace()->focusedView() == view;
      if (entered && !alreadyFocused) {
        bool allow = true;
        Workspace* workspace = view->workspace();
        if (workspace != nullptr
            && view->tiled()
            && workspace->group() != nullptr
            && workspace->group()->output() != nullptr) {
          const int column = workspace->layout().columnOf(view);
          const int viewportWidth =
              std::max(1, workspace->group()->output()->usableArea().width - 2 * config().layoutEdgePad());
          const double amount = workspace->layout().scrollAmountToEnsureVisible(column, viewportWidth);
          if (const auto& maxScroll = config().input.focus.followsMouseMaxScroll) {
            if (amount > *maxScroll) {
              allow = false;
            }
          }
        }
        if (allow) {
          m_server->focusView(view, true);
          // Scroll may have moved another surface under the cursor; refresh hit-test for
          // pointer notify only. Keyboard focus stays on the entered view until a real enter.
          view = m_server->viewAt(m_cursor->x, m_cursor->y, &surface, &sx, &sy, &layer);
        }
      }
    }

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
        &m_grabbedView->sceneTree()->node, static_cast<int>(m_cursor->x - m_grabX),
        static_cast<int>(m_cursor->y - m_grabY)
    );
    if (m_mode == CursorMode::MoveTile) {
      clipGrabbedViewToOutput();
    }
  }

  void Cursor::clipGrabbedViewToOutput() {
    if (m_grabbedView == nullptr) {
      return;
    }
    const wlr_box& geometry = m_grabbedView->toplevel()->base->geometry;
    const wlr_box target{
        .x = m_grabbedView->sceneTree()->node.x,
        .y = m_grabbedView->sceneTree()->node.y,
        .width = geometry.width,
        .height = geometry.height,
    };
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (output == nullptr) {
      return;
    }
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), output->wlr(), &outputBox);
    const int border = m_grabbedView->tiled() ? config().appearance.totalBorderWidth() : 0;
    wlr_box decorated = target;
    decorated.x -= border;
    decorated.y -= border;
    decorated.width += 2 * border;
    decorated.height += 2 * border;
    wlr_box intersection{};
    if (!wlr_box_intersection(&intersection, &decorated, &outputBox)) {
      wlr_scene_node_set_enabled(&m_grabbedView->sceneTree()->node, false);
      return;
    }
    wlr_scene_node_set_enabled(&m_grabbedView->sceneTree()->node, true);
    m_grabbedView->setOutputClip(&intersection, target, outputBox);
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
    const wlr_box usable = output->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }
    if (m_dropWorkspace != nullptr && m_dropWorkspace != workspace && m_dropWorkspace->layout().insertGap() >= 0) {
      m_dropWorkspace->layout().clearInsertGap();
      m_dropWorkspace->arrange(false);
    }
    if (workspace->layout().insertGap() >= 0) {
      workspace->layout().clearInsertGap();
      workspace->arrange(false);
    }
    const int edgePad = config().layoutEdgePad();
    const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
    const int columnCount = static_cast<int>(workspace->layout().columns().size());
    const double layoutX = m_cursor->x - usable.x - edgePad + workspace->visualScroll();

    for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
      const int columnX = workspace->layout().columnX(columnIndex, viewportWidth);
      const int columnWidth = workspace->layout().columnWidth(columnIndex, viewportWidth);
      if (layoutX < columnX + columnWidth * 0.2 || layoutX > columnX + columnWidth * 0.8) {
        continue;
      }
      const Column& column = workspace->layout().columns()[static_cast<size_t>(columnIndex)];
      int nearestRow = 0;
      double rowDistance = std::abs(m_cursor->y - (usable.y + edgePad));
      for (int row = 1; row <= static_cast<int>(column.views.size()); ++row) {
        const int boundary = row == static_cast<int>(column.views.size())
            ? usable.y + usable.height - edgePad
            : workspace->layout().targetBox(column.views[static_cast<size_t>(row)]).y - config().layoutGap() / 2;
        const double distance = std::abs(m_cursor->y - boundary);
        if (distance < rowDistance) {
          nearestRow = row;
          rowDistance = distance;
        }
      }
      m_dropWorkspace = workspace;
      m_dropColumn = columnIndex;
      m_dropRow = nearestRow;
      m_server->insertHint().showRow(workspace, columnIndex, nearestRow);
      wlr_scene_node_raise_to_top(&m_grabbedView->sceneTree()->node);
      return;
    }

    int nearestGap = 0;
    double nearestDistance = std::abs(layoutX);
    for (int gap = 1; gap <= columnCount; ++gap) {
      const int boundary = gap == columnCount
          ? workspace->layout().columnX(gap, viewportWidth) - config().layoutGap()
          : workspace->layout().columnX(gap, viewportWidth) - config().layoutGap() / 2;
      const double distance = std::abs(layoutX - boundary);
      if (distance < nearestDistance) {
        nearestGap = gap;
        nearestDistance = distance;
      }
    }

    m_dropWorkspace = workspace;
    m_dropColumn = nearestGap;
    m_dropRow = -1;
    m_server->insertHint().show(workspace, nearestGap);
    wlr_scene_node_raise_to_top(&m_grabbedView->sceneTree()->node);
  }

  void Cursor::finishTileMove() {
    m_server->hideInsertHint();
    View* view = m_grabbedView;
    Workspace* target = m_dropWorkspace != nullptr ? m_dropWorkspace : m_dragSourceWorkspace;
    const int column = std::max(0, m_dropColumn);
    if (target != nullptr) {
      target->layout().clearInsertGap();
    }
    if (view != nullptr && view->mapped() && target != nullptr) {
      if (view->workspace() != target) {
        view->setWorkspace(target);
        target->layoutDetach(view);
      }
      if (m_dropRow >= 0) {
        target->layout().insertViewIntoColumn(view, column, m_dropRow);
      } else {
        target->layout().insertView(view, column);
      }
      target->arrange();
      m_server->focusView(view);
    }
    resetMode();
  }

  void Cursor::finishFloatMove() {
    View* view = m_grabbedView;
    if (view != nullptr && view->mapped()) {
      const int x = view->sceneTree()->node.x;
      const int y = view->sceneTree()->node.y;
      // Own the drop output so per-frame home-output culling does not hide the window.
      wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), m_cursor->x, m_cursor->y);
      Output* output = m_server->outputFromWlr(wlrOutput);
      if (output != nullptr && output->workspaceGroup() != nullptr) {
        if (Workspace* target = output->workspaceGroup()->active()) {
          if (view->workspace() != target) {
            view->setWorkspace(target);
            view->setPosition(x, y);
          }
        }
      }
      wlr_scene_node_set_enabled(&view->sceneTree()->node, true);
      m_server->focusView(view, false);
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

    wlr_box* geo = &m_grabbedView->toplevel()->base->geometry;
    wlr_scene_node_set_position(&m_grabbedView->sceneTree()->node, newLeft - geo->x, newTop - geo->y);
    wlr_xdg_toplevel_set_size(m_grabbedView->toplevel(), newRight - newLeft, newBottom - newTop);
  }

  uint32_t Cursor::tileResizeEdges(View* view) const {
    if (view == nullptr || view->workspace() == nullptr) {
      return WLR_EDGE_RIGHT;
    }
    const wlr_box box = view->workspace()->layout().targetBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return WLR_EDGE_RIGHT;
    }
    const double cx = m_cursor->x;
    const double cy = m_cursor->y;
    const double distLeft = std::abs(cx - box.x);
    const double distRight = std::abs(cx - (box.x + box.width));
    const double distTop = std::abs(cy - box.y);
    const double distBottom = std::abs(cy - (box.y + box.height));
    const double nearestH = std::min(distLeft, distRight);
    const double nearestV = std::min(distTop, distBottom);

    bool allowVertical = false;
    const int columnIndex = view->workspace()->layout().columnOf(view);
    if (columnIndex >= 0) {
      const auto& columns = view->workspace()->layout().columns();
      allowVertical =
          columnIndex < static_cast<int>(columns.size()) && columns[static_cast<size_t>(columnIndex)].views.size() >= 2;
    }

    if (!allowVertical || nearestH <= nearestV) {
      return distLeft <= distRight ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
    }
    return distTop <= distBottom ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
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

  void Cursor::processResizeTile() {
    if (m_resizeWorkspace == nullptr
        || m_resizeWorkspace->group() == nullptr
        || m_resizeWorkspace->group()->output() == nullptr
        || m_resizeColumn < 0
        || m_grabbedView == nullptr) {
      resetMode();
      return;
    }

    ScrollingLayout& layout = m_resizeWorkspace->layout();
    const int viewportWidth =
        std::max(1, m_resizeWorkspace->group()->output()->usableArea().width - 2 * config().layoutEdgePad());
    const int availableHeight =
        std::max(1, m_resizeWorkspace->group()->output()->usableArea().height - 2 * config().layoutEdgePad());
    const double dx = m_cursor->x - m_resizeStartX;
    const double dy = m_cursor->y - m_resizeStartY;

    auto columnMinWidth = [&](int columnIndex) {
      int minWidth = static_cast<int>(std::lround(0.15 * viewportWidth));
      if (columnIndex < 0 || columnIndex >= static_cast<int>(layout.columns().size())) {
        return minWidth;
      }
      for (View* view : layout.columns()[static_cast<size_t>(columnIndex)].views) {
        if (view != nullptr) {
          minWidth = std::max(minWidth, xdgSizeHints(view->toplevel()).minWidth);
        }
      }
      return std::min(minWidth, viewportWidth);
    };
    auto columnMaxWidth = [&](int columnIndex) {
      int maxWidth = viewportWidth;
      if (columnIndex < 0 || columnIndex >= static_cast<int>(layout.columns().size())) {
        return maxWidth;
      }
      bool any = false;
      int clientMax = 0;
      for (View* view : layout.columns()[static_cast<size_t>(columnIndex)].views) {
        if (view == nullptr) {
          continue;
        }
        const int hintMax = xdgSizeHints(view->toplevel()).maxWidth;
        if (hintMax > 0) {
          clientMax = any ? std::min(clientMax, hintMax) : hintMax;
          any = true;
        }
      }
      if (any) {
        maxWidth = std::min(maxWidth, clientMax);
      }
      return std::max(maxWidth, columnMinWidth(columnIndex));
    };

    if ((m_resizeEdges & WLR_EDGE_RIGHT) != 0) {
      // Pin left edge; only this column's width changes (neighbors untouched).
      const int newWidth = std::clamp(
          m_resizeStartWidthPx + static_cast<int>(std::lround(dx)), columnMinWidth(m_resizeColumn),
          columnMaxWidth(m_resizeColumn)
      );
      layout.setWidthFraction(m_resizeColumn, static_cast<double>(newWidth) / viewportWidth);
      if (m_resizeSoloHorizontal) {
        // Keep the left edge where the full-width column was aligned.
        layout.setScroll(m_resizeStartScroll);
      }
    } else if ((m_resizeEdges & WLR_EDGE_LEFT) != 0) {
      if (m_resizeColumn > 0 && !m_resizeSoloHorizontal) {
        // Shared boundary with previous column: move both widths, keep pair span fixed.
        const int gap = config().layoutGap();
        const int pair = m_resizeStartPrevWidthPx + gap + m_resizeStartWidthPx;
        const int minPrev = columnMinWidth(m_resizeColumn - 1);
        const int minCur = columnMinWidth(m_resizeColumn);
        const int maxPrev = columnMaxWidth(m_resizeColumn - 1);
        const int maxCur = columnMaxWidth(m_resizeColumn);
        const int newPrev = std::clamp(
            m_resizeStartPrevWidthPx + static_cast<int>(std::lround(dx)), std::max(minPrev, pair - gap - maxCur),
            std::min(maxPrev, pair - gap - minCur)
        );
        const int newCur = pair - gap - newPrev;
        layout.setWidthFraction(m_resizeColumn - 1, static_cast<double>(newPrev) / viewportWidth);
        layout.setWidthFraction(m_resizeColumn, static_cast<double>(newCur) / viewportWidth);
      } else {
        // Column 0 or full-width: pin the right edge, move only this column via width + scroll.
        const int newWidth = std::clamp(
            m_resizeStartWidthPx - static_cast<int>(std::lround(dx)), columnMinWidth(m_resizeColumn),
            columnMaxWidth(m_resizeColumn)
        );
        layout.setWidthFraction(m_resizeColumn, static_cast<double>(newWidth) / viewportWidth);
        layout.setScroll(m_resizeStartScroll + static_cast<double>(newWidth - m_resizeStartWidthPx));
      }
    }

    if ((m_resizeEdges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0 && m_resizeRow >= 0) {
      const Column& column = layout.columns()[static_cast<size_t>(m_resizeColumn)];
      const int rowCount = static_cast<int>(column.views.size());
      // Vertical resize only with 2+ stacked windows (solo + gap weights get stuck later).
      if (rowCount >= 2) {
        const int gap = config().layoutGap();
        const int gapsTotal = std::max(0, rowCount - 1) * gap;
        const int stackHeight = std::max(rowCount, availableHeight - gapsTotal);
        if (stackHeight > 0) {
          constexpr double kMinWindow = 0.05;
          double totalWeight = std::max(0.0, column.topGapWeight) + std::max(0.0, column.bottomGapWeight);
          for (double weight : column.heightWeights) {
            totalWeight += std::max(kMinWindow, weight);
          }
          totalWeight = std::max(kMinWindow, totalWeight);

          auto minWindowWeight = [&](View* view) {
            if (view == nullptr) {
              return kMinWindow;
            }
            const double fromHints =
                static_cast<double>(xdgSizeHints(view->toplevel()).minHeight) / stackHeight * totalWeight;
            return std::max(kMinWindow, fromHints);
          };

          const double pair = std::max(kMinWindow, m_resizeStartUpperWeight + m_resizeStartLowerWeight);
          const double deltaWeight = dy / static_cast<double>(stackHeight) * pair;

          auto splitWindows = [&](double startUpper, double /*startLower*/, double delta, double minUpper,
                                  double minLower) {
            double upper = startUpper + delta;
            double lower = pair - upper;
            if (upper < minUpper) {
              upper = minUpper;
              lower = pair - upper;
            }
            if (lower < minLower) {
              lower = minLower;
              upper = pair - lower;
            }
            return std::pair{upper, lower};
          };
          auto splitGapAndWindow = [&](double startGap, double /*startWindow*/, double deltaGap, double minWindow) {
            double gapWeight = startGap + deltaGap;
            double windowWeight = pair - gapWeight;
            if (gapWeight < 0.0) {
              gapWeight = 0.0;
              windowWeight = pair;
            }
            if (windowWeight < minWindow) {
              windowWeight = minWindow;
              gapWeight = pair - windowWeight;
              if (gapWeight < 0.0) {
                gapWeight = 0.0;
                windowWeight = pair;
              }
            }
            return std::pair{gapWeight, windowWeight};
          };

          if ((m_resizeEdges & WLR_EDGE_TOP) != 0) {
            if (m_resizeRow == 0) {
              // Top edge of first window: trade with top gap only.
              const double minWindow = minWindowWeight(column.views[0]);
              const auto [gapWeight, windowWeight] =
                  splitGapAndWindow(m_resizeStartUpperWeight, m_resizeStartLowerWeight, deltaWeight, minWindow);
              layout.setTopGapWeight(m_resizeColumn, gapWeight);
              layout.setHeightWeight(m_resizeColumn, 0, windowWeight);
            } else if (m_resizeUpperRow >= 0) {
              const double minUpper = minWindowWeight(column.views[static_cast<size_t>(m_resizeUpperRow)]);
              const double minLower = minWindowWeight(column.views[static_cast<size_t>(m_resizeRow)]);
              const auto [upper, lower] =
                  splitWindows(m_resizeStartUpperWeight, m_resizeStartLowerWeight, deltaWeight, minUpper, minLower);
              layout.setRowBoundary(m_resizeColumn, m_resizeUpperRow, upper, lower);
            }
          } else if ((m_resizeEdges & WLR_EDGE_BOTTOM) != 0) {
            if (m_resizeRow + 1 >= rowCount) {
              // Bottom edge of last window: trade with bottom gap only.
              // Dragging down with no gap must not steal height from the window above.
              const double minWindow = minWindowWeight(column.views[static_cast<size_t>(m_resizeRow)]);
              const auto [gapWeight, windowWeight] =
                  splitGapAndWindow(m_resizeStartLowerWeight, m_resizeStartUpperWeight, -deltaWeight, minWindow);
              layout.setHeightWeight(m_resizeColumn, m_resizeRow, windowWeight);
              layout.setBottomGapWeight(m_resizeColumn, gapWeight);
            } else if (m_resizeUpperRow >= 0) {
              const double minUpper = minWindowWeight(column.views[static_cast<size_t>(m_resizeUpperRow)]);
              const double minLower = minWindowWeight(column.views[static_cast<size_t>(m_resizeRow + 1)]);
              const auto [upper, lower] =
                  splitWindows(m_resizeStartUpperWeight, m_resizeStartLowerWeight, deltaWeight, minUpper, minLower);
              layout.setRowBoundary(m_resizeColumn, m_resizeUpperRow, upper, lower);
            }
          }
        }
      }
    }

    wlr_xdg_toplevel_set_maximized(m_grabbedView->toplevel(), false);
    m_resizeWorkspace->arrange(false);
  }

} // namespace umbriel
