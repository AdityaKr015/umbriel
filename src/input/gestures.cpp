#include "input/gestures.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("gestures");

    // Tuning constants — file-local, no config keys.
    constexpr double kAxisLockPx = 16.0;
    constexpr double kScrollFactor = 3.0;
    constexpr double kSwitchDistancePx = 300.0;
    constexpr double kCommitProgress = 0.35;
    constexpr double kCommitVelocityPxMs = 0.9;
    constexpr double kOverscrollCompress = 0.15;
    constexpr double kOverscrollMaxWs = 0.08;
  } // namespace

  // ----- trampolines (same pattern as Cursor) -----

  void Gestures::onSwipeBegin(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_swipeBegin);
    self->handleSwipeBegin(data);
  }
  void Gestures::onSwipeUpdate(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_swipeUpdate);
    self->handleSwipeUpdate(data);
  }
  void Gestures::onSwipeEnd(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_swipeEnd);
    self->handleSwipeEnd(data);
  }
  void Gestures::onPinchBegin(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_pinchBegin);
    self->handlePinchBegin(data);
  }
  void Gestures::onPinchUpdate(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_pinchUpdate);
    self->handlePinchUpdate(data);
  }
  void Gestures::onPinchEnd(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_pinchEnd);
    self->handlePinchEnd(data);
  }
  void Gestures::onHoldBegin(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_holdBegin);
    self->handleHoldBegin(data);
  }
  void Gestures::onHoldEnd(wl_listener* listener, void* data) {
    Gestures* self;
    self = wl_container_of(listener, self, m_holdEnd);
    self->handleHoldEnd(data);
  }

  Gestures::Gestures(Server& server) : m_server(&server) {
    wlr_cursor* cursor = m_server->cursor()->wlr();

    m_swipeBegin.notify = onSwipeBegin;
    wl_signal_add(&cursor->events.swipe_begin, &m_swipeBegin);
    m_swipeUpdate.notify = onSwipeUpdate;
    wl_signal_add(&cursor->events.swipe_update, &m_swipeUpdate);
    m_swipeEnd.notify = onSwipeEnd;
    wl_signal_add(&cursor->events.swipe_end, &m_swipeEnd);
    m_pinchBegin.notify = onPinchBegin;
    wl_signal_add(&cursor->events.pinch_begin, &m_pinchBegin);
    m_pinchUpdate.notify = onPinchUpdate;
    wl_signal_add(&cursor->events.pinch_update, &m_pinchUpdate);
    m_pinchEnd.notify = onPinchEnd;
    wl_signal_add(&cursor->events.pinch_end, &m_pinchEnd);
    m_holdBegin.notify = onHoldBegin;
    wl_signal_add(&cursor->events.hold_begin, &m_holdBegin);
    m_holdEnd.notify = onHoldEnd;
    wl_signal_add(&cursor->events.hold_end, &m_holdEnd);
  }

  Gestures::~Gestures() {
    wl_list_remove(&m_swipeBegin.link);
    wl_list_remove(&m_swipeUpdate.link);
    wl_list_remove(&m_swipeEnd.link);
    wl_list_remove(&m_pinchBegin.link);
    wl_list_remove(&m_pinchUpdate.link);
    wl_list_remove(&m_pinchEnd.link);
    wl_list_remove(&m_holdBegin.link);
    wl_list_remove(&m_holdEnd.link);
  }

  void Gestures::cancelForOutput(Output* output) {
    if (m_output == output && (m_state == State::Pending || m_state == State::Scroll || m_state == State::Switch)) {
      // Hard reset — do NOT call into workspace/group objects (they may be mid-destruction).
      m_output = nullptr;
      m_scrollWorkspace = nullptr;
      m_switchGroup = nullptr;
      m_state = State::Idle;
    }
  }

  void Gestures::cancelActive() {
    switch (m_state) {
    case State::Scroll:
      finishScroll(true);
      break;
    case State::Switch:
      finishSwitch(true);
      break;
    case State::Forward:
      // Forward a cancel end so clients see the end.
      wlr_pointer_gestures_v1_send_swipe_end(m_server->pointerGestures(), m_server->seat()->wlr(), 0, true);
      m_state = State::Idle;
      break;
    case State::Pending:
    case State::Idle:
      m_state = State::Idle;
      break;
    }
  }

  // Restore layout/slide state without sending any protocol events.
  // Used when the session locks mid-gesture.
  void Gestures::silentCancel() {
    switch (m_state) {
    case State::Scroll:
      finishScroll(true);
      break;
    case State::Switch:
      finishSwitch(true);
      break;
    case State::Forward:
    case State::Pending:
    case State::Idle:
      m_state = State::Idle;
      break;
    }
  }

  // ===== Swipe handlers =====

  void Gestures::handleSwipeBegin(void* data) {
    auto* event = static_cast<wlr_pointer_swipe_begin_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }
    if (m_state != State::Idle) {
      cancelActive();
    }
    if (event->fingers == 3) {
      m_state = State::Pending;
      m_accumX = 0;
      m_accumY = 0;
      m_output = nullptr;
    } else {
      m_state = State::Forward;
      wlr_pointer_gestures_v1_send_swipe_begin(
          m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->fingers
      );
    }
  }

  void Gestures::handleSwipeUpdate(void* data) {
    auto* event = static_cast<wlr_pointer_swipe_update_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }

    switch (m_state) {
    case State::Forward:
      wlr_pointer_gestures_v1_send_swipe_update(
          m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->dx, event->dy
      );
      return;

    case State::Pending: {
      m_accumX += event->dx;
      m_accumY += event->dy;
      const double maxAccum = std::max(std::abs(m_accumX), std::abs(m_accumY));
      if (maxAccum < kAxisLockPx) {
        return; // Not enough travel to decide axis.
      }
      // Resolve output.
      wlr_output* wlrOut = m_server->preferredOutput();
      Output* out = m_server->outputFromWlr(wlrOut);
      if (out == nullptr || out->workspaceGroup() == nullptr) {
        m_state = State::Idle;
        return;
      }
      m_output = out;

      if (std::abs(m_accumX) > std::abs(m_accumY)) {
        // ----- Horizontal lock → Scroll -----
        Workspace* ws = out->workspaceGroup()->active();
        if (ws == nullptr || ws->layout().columns().empty()) {
          m_state = State::Idle;
          return;
        }
        m_scrollWorkspace = ws;
        m_viewportWidth = std::max(1, out->usableArea().width - 2 * config().layoutEdgePad());
        m_scrollStart = ws->visualScroll();
        ws->layout().setScroll(m_scrollStart);
        ws->arrange(false);
        m_state = State::Scroll;
      } else {
        // ----- Vertical lock → Switch -----
        WorkspaceGroup* group = out->workspaceGroup();
        const size_t idx = group->active()->index();
        m_hasPrev = idx > 0;
        m_hasNext = idx + 1 < WorkspaceGroup::kDefaultCount;
        if (!group->slideBegin(m_hasPrev, m_hasNext)) {
          m_state = State::Idle;
          return;
        }
        m_switchGroup = group;
        m_progress = 0;
        m_velocity = 0;
        m_lastTimeMsec = event->time_msec;
        m_state = State::Switch;
      }
      return;
    }

    case State::Scroll: {
      // Abort if active workspace changed under us.
      Output* out = m_server->outputFromWlr(m_server->preferredOutput());
      if (out == nullptr || out->workspaceGroup() == nullptr || out->workspaceGroup()->active() != m_scrollWorkspace) {
        m_state = State::Idle;
        m_scrollWorkspace = nullptr;
        return;
      }
      m_accumX += event->dx;
      // Natural: fingers left → content moves left → scroll increases.
      double target = m_scrollStart - m_accumX * kScrollFactor;
      const auto maxScroll = static_cast<double>(m_scrollWorkspace->layout().maxScroll(m_viewportWidth));
      if (target < 0) {
        target = std::max(target * kOverscrollCompress, -0.1 * m_viewportWidth);
      }
      if (target > maxScroll) {
        target = std::min(maxScroll + (target - maxScroll) * kOverscrollCompress, maxScroll + 0.1 * m_viewportWidth);
      }
      m_scrollWorkspace->layout().setScroll(target);
      m_scrollWorkspace->arrange(false);
      return;
    }

    case State::Switch: {
      // Abort if group changed.
      Output* out = m_server->outputFromWlr(m_server->preferredOutput());
      if (out == nullptr || out->workspaceGroup() != m_switchGroup) {
        m_switchGroup->slideFinish();
        m_switchGroup = nullptr;
        m_state = State::Idle;
        return;
      }
      m_accumY += event->dy;
      // Natural: swipe up (negative dy) → next workspace (positive progress).
      double p = -m_accumY / kSwitchDistancePx;
      const double lo = m_hasPrev ? -1.0 : 0.0;
      const double hi = m_hasNext ? 1.0 : 0.0;
      if (p < lo) {
        p = std::max(lo + (p - lo) * kOverscrollCompress, lo - kOverscrollMaxWs);
      }
      if (p > hi) {
        p = std::min(hi + (p - hi) * kOverscrollCompress, hi + kOverscrollMaxWs);
      }
      const uint32_t dt = std::max(1U, event->time_msec - m_lastTimeMsec);
      m_velocity = 0.75 * m_velocity + 0.25 * (-event->dy / static_cast<double>(dt));
      m_lastTimeMsec = event->time_msec;
      m_progress = p;
      m_switchGroup->slideApply(p);
      return;
    }

    case State::Idle:
      return;
    }
  }

  void Gestures::handleSwipeEnd(void* data) {
    auto* event = static_cast<wlr_pointer_swipe_end_event*>(data);
    m_server->notifyIdleActivity();

    if (m_server->sessionLocked()) {
      silentCancel();
      return;
    }

    switch (m_state) {
    case State::Forward:
      wlr_pointer_gestures_v1_send_swipe_end(
          m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->cancelled
      );
      m_state = State::Idle;
      return;

    case State::Pending:
      m_state = State::Idle;
      return;

    case State::Scroll:
      finishScroll(event->cancelled);
      return;

    case State::Switch:
      finishSwitch(event->cancelled);
      return;

    case State::Idle:
      return;
    }
  }

  // ===== Scroll finish (Step 5) =====

  void Gestures::finishScroll(bool cancelled) {
    if (m_scrollWorkspace == nullptr) {
      m_state = State::Idle;
      return;
    }
    if (cancelled) {
      m_scrollWorkspace->layout().setScroll(m_scrollStart);
      m_scrollWorkspace->arrange(true);
    } else {
      // Snap to the column nearest the viewport center.
      const auto& layout = m_scrollWorkspace->layout();
      const auto maxScroll = static_cast<double>(layout.maxScroll(m_viewportWidth));
      const double currentScroll = std::clamp(layout.scroll(), 0.0, maxScroll);
      const double center = currentScroll + m_viewportWidth / 2.0;
      int best = -1;
      double bestDist = 1e18;
      for (int i = 0; i < static_cast<int>(layout.columns().size()); ++i) {
        const double colCenter =
            static_cast<double>(layout.columnX(i, m_viewportWidth)) + layout.columnWidth(i, m_viewportWidth) / 2.0;
        const double dist = std::abs(colCenter - center);
        if (dist < bestDist) {
          bestDist = dist;
          best = i;
        }
      }

      if (best >= 0) {
        View* focused = m_scrollWorkspace->focusedView();
        if (focused != nullptr && layout.columnOf(focused) == best) {
          // Snap back / cancel: nearest column is already focused.
          m_scrollWorkspace->ensureFocusedVisible();
          m_scrollWorkspace->arrange(true);
        } else if (
            best < static_cast<int>(layout.columns().size())
            && !layout.columns()[static_cast<size_t>(best)].views.empty()
        ) {
          View* target = layout.columns()[static_cast<size_t>(best)].views.front();
          m_server->focusView(target, FocusReason::Directional);
        } else {
          m_scrollWorkspace->ensureFocusedVisible();
          m_scrollWorkspace->arrange(true);
        }
      } else {
        m_scrollWorkspace->ensureFocusedVisible();
        m_scrollWorkspace->arrange(true);
      }
    }
    m_scrollWorkspace = nullptr;
    m_state = State::Idle;
  }

  // ===== Switch finish (Step 6) =====

  void Gestures::finishSwitch(bool cancelled) {
    if (m_switchGroup == nullptr) {
      m_state = State::Idle;
      return;
    }
    int delta = 0;
    if (!cancelled) {
      const double lo = m_hasPrev ? -1.0 : 0.0;
      const double hi = m_hasNext ? 1.0 : 0.0;
      const double clamped = std::clamp(m_progress, lo, hi);
      if (std::abs(clamped) >= kCommitProgress) {
        delta = clamped > 0 ? 1 : -1;
      } else if (std::abs(m_velocity) >= kCommitVelocityPxMs && m_velocity * clamped > 0) {
        delta = clamped > 0 ? 1 : -1;
      }
    }
    m_switchGroup->slideSettle(delta);
    if (delta != 0) {
      m_server->cursor()->clearConstraint();
      m_server->refocus(m_output);
    }
    m_switchGroup = nullptr;
    m_state = State::Idle;
  }

  // ===== Pinch handlers (forward unconditionally) =====

  void Gestures::handlePinchBegin(void* data) {
    auto* event = static_cast<wlr_pointer_pinch_begin_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_pinch_begin(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->fingers
    );
  }

  void Gestures::handlePinchUpdate(void* data) {
    auto* event = static_cast<wlr_pointer_pinch_update_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_pinch_update(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->dx, event->dy, event->scale,
        event->rotation
    );
  }

  void Gestures::handlePinchEnd(void* data) {
    auto* event = static_cast<wlr_pointer_pinch_end_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_pinch_end(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->cancelled
    );
  }

  // ===== Hold handlers (forward unconditionally) =====

  void Gestures::handleHoldBegin(void* data) {
    auto* event = static_cast<wlr_pointer_hold_begin_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_hold_begin(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->fingers
    );
  }

  void Gestures::handleHoldEnd(void* data) {
    auto* event = static_cast<wlr_pointer_hold_end_event*>(data);
    m_server->notifyIdleActivity();
    if (m_server->sessionLocked()) {
      return;
    }
    wlr_pointer_gestures_v1_send_hold_end(
        m_server->pointerGestures(), m_server->seat()->wlr(), event->time_msec, event->cancelled
    );
  }

} // namespace umbriel
