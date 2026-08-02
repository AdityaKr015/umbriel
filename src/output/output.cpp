#include "output/output.hpp"

#include "server/server.hpp"
#include "wlr.hpp"

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
  if (wlr_output_mode* mode = wlr_output_preferred_mode(m_output)) {
    wlr_output_state_set_mode(&state, mode);
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

void Output::handleFrame() {
  if (!wlr_scene_output_commit(m_sceneOutput, nullptr)) {
    return;
  }

  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(m_sceneOutput, &now);
}

void Output::handleRequestState(void* data) {
  auto* event = static_cast<wlr_output_event_request_state*>(data);
  wlr_output_commit_state(m_output, event->state);
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
