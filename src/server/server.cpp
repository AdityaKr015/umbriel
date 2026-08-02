#include "server/server.hpp"

#include "output/output.hpp"
#include "wlr.hpp"

#include <cstdlib>
#include <stdexcept>

namespace umbriel {

Server::Server() {
  m_display = wl_display_create();
  if (m_display == nullptr) {
    throw std::runtime_error("failed to create wl_display");
  }

  m_backend = wlr_backend_autocreate(wl_display_get_event_loop(m_display), nullptr);
  if (m_backend == nullptr) {
    throw std::runtime_error("failed to create wlr_backend");
  }

  m_renderer = fx_renderer_create(m_backend);
  if (m_renderer == nullptr) {
    throw std::runtime_error("failed to create fx_renderer");
  }
  wlr_renderer_init_wl_display(m_renderer, m_display);

  m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);
  if (m_allocator == nullptr) {
    throw std::runtime_error("failed to create wlr_allocator");
  }

  m_compositor = wlr_compositor_create(m_display, 5, m_renderer);
  wlr_subcompositor_create(m_display);
  wlr_data_device_manager_create(m_display);

  m_outputLayout = wlr_output_layout_create(m_display);
  m_scene = wlr_scene_create();
  m_sceneLayout = wlr_scene_attach_output_layout(m_scene, m_outputLayout);

  m_newOutput.notify = onNewOutput;
  wl_signal_add(&m_backend->events.new_output, &m_newOutput);
}

Server::~Server() {
  wl_list_remove(&m_newOutput.link);
  m_outputs.clear();
  wl_display_destroy_clients(m_display);
  wlr_scene_node_destroy(&m_scene->tree.node);
  wlr_allocator_destroy(m_allocator);
  wlr_renderer_destroy(m_renderer);
  wlr_backend_destroy(m_backend);
  wl_display_destroy(m_display);
}

bool Server::start() {
  const char* socket = wl_display_add_socket_auto(m_display);
  if (socket == nullptr) {
    wlr_log(WLR_ERROR, "failed to add Wayland socket");
    return false;
  }

  if (!wlr_backend_start(m_backend)) {
    wlr_log(WLR_ERROR, "failed to start backend");
    return false;
  }

  setenv("WAYLAND_DISPLAY", socket, true);
  wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
  return true;
}

void Server::run() { wl_display_run(m_display); }

void Server::stop() { wl_display_terminate(m_display); }

void Server::onNewOutput(wl_listener* listener, void* data) {
  Server* self = wl_container_of(listener, self, m_newOutput);
  self->addOutput(static_cast<wlr_output*>(data));
}

void Server::addOutput(wlr_output* output) {
  m_outputs.push_back(std::make_unique<Output>(*this, output));
}

void Server::removeOutput(Output* output) {
  std::erase_if(m_outputs, [output](const std::unique_ptr<Output>& entry) {
    return entry.get() == output;
  });
}

} // namespace umbriel
