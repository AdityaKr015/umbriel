#include "output/output.h"

#include "server/server.h"
#include "wlr.h"

#include <ctime>

namespace umbriel {

Output::Output(Server& server, wlr_output* output) : m_server(&server), m_output(output) {
  wlr_output_init_render(m_output, m_server->allocator(), m_server->renderer());

  m_frame.notify = onFrame;
  wl_signal_add(&m_output->events.frame, &m_frame);

  m_requestState.notify = onRequestState;
  wl_signal_add(&m_output->events.request_state, &m_requestState);

  m_destroy.notify = onDestroy;
  wl_signal_add(&m_output->events.destroy, &m_destroy);

  wlr_output_state state{};
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  // Nested Wayland outputs get their real size from the parent configure
  // (request_state). Setting preferred mode here races that path.
  if (!wlr_output_is_wl(m_output)) {
    if (wlr_output_mode* mode = wlr_output_preferred_mode(m_output)) {
      wlr_output_state_set_mode(&state, mode);
    }
  }
  wlr_output_commit_state(m_output, &state);
  wlr_output_state_finish(&state);

  wlr_output_layout_output* layoutOutput = wlr_output_layout_add_auto(m_server->outputLayout(), m_output);
  m_sceneOutput = wlr_scene_output_create(m_server->scene(), m_output);
  wlr_scene_output_layout_add_output(m_server->sceneLayout(), layoutOutput, m_sceneOutput);
}

Output::~Output() {
  if (m_frame.link.next != nullptr) {
    wl_list_remove(&m_frame.link);
    wl_list_remove(&m_requestState.link);
    wl_list_remove(&m_destroy.link);
  }
}

void Output::onFrame(wl_listener* listener, void* /*data*/) {
  Output* self = wl_container_of(listener, self, m_frame);
  self->handleFrame();
}

void Output::onRequestState(wl_listener* listener, void* data) {
  Output* self = wl_container_of(listener, self, m_requestState);
  self->handleRequestState(data);
}

void Output::onDestroy(wl_listener* listener, void* /*data*/) {
  Output* self = wl_container_of(listener, self, m_destroy);
  self->handleDestroy();
}

void Output::applyMode(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }

  wlr_output_state state{};
  wlr_output_state_init(&state);
  wlr_output_state_set_custom_mode(&state, width, height, 0);
  if (!wlr_output_commit_state(m_output, &state)) {
    wlr_log(WLR_ERROR, "failed to commit output mode %dx%d for '%s'", width, height, m_output->name);
  }
  wlr_output_state_finish(&state);
  wlr_output_schedule_frame(m_output);
}

void Output::handleFrame() {
  if (m_hasDeferredMode) {
    m_hasDeferredMode = false;
    applyMode(m_deferredWidth, m_deferredHeight);
  }

  if (!wlr_scene_output_needs_frame(m_sceneOutput) || m_output->width <= 0 || m_output->height <= 0) {
    return;
  }

  m_inFrame = true;
  const bool ok = wlr_scene_output_commit(m_sceneOutput, nullptr);
  m_inFrame = false;

  if (m_hasDeferredMode) {
    m_hasDeferredMode = false;
    applyMode(m_deferredWidth, m_deferredHeight);
    return;
  }

  if (!ok) {
    wlr_output_schedule_frame(m_output);
    return;
  }

  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(m_sceneOutput, &now);
}

void Output::handleRequestState(void* data) {
  auto* event = static_cast<wlr_output_event_request_state*>(data);

  // Parent configure can arrive while we are flushing a frame commit. Applying a
  // mode change mid-frame makes the wayland backend reject the primary buffer.
  if (m_inFrame && (event->state->committed & WLR_OUTPUT_STATE_MODE) != 0
      && event->state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
    m_deferredWidth = event->state->custom_mode.width;
    m_deferredHeight = event->state->custom_mode.height;
    m_hasDeferredMode = true;
    return;
  }

  if (!wlr_output_commit_state(m_output, event->state)) {
    wlr_log(WLR_ERROR, "failed to commit requested output state for '%s'", m_output->name);
    return;
  }
  wlr_output_schedule_frame(m_output);
}

void Output::handleDestroy() {
  wl_list_remove(&m_frame.link);
  wl_list_remove(&m_requestState.link);
  wl_list_remove(&m_destroy.link);
  m_frame.link.next = nullptr;
  m_requestState.link.next = nullptr;
  m_destroy.link.next = nullptr;
  m_server->removeOutput(this);
}

} // namespace umbriel
