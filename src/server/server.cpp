#include "server/server.h"

#include "input/cursor.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "output/output.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace umbriel {

Server::Server() {
  m_nested = std::getenv("WAYLAND_DISPLAY") != nullptr || std::getenv("WAYLAND_SOCKET") != nullptr
      || std::getenv("DISPLAY") != nullptr;

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

  m_xdgShell = wlr_xdg_shell_create(m_display, 3);
  m_newXdgToplevel.notify = onNewXdgToplevel;
  wl_signal_add(&m_xdgShell->events.new_toplevel, &m_newXdgToplevel);
  m_newXdgPopup.notify = onNewXdgPopup;
  wl_signal_add(&m_xdgShell->events.new_popup, &m_newXdgPopup);

  m_cursor = std::make_unique<Cursor>(*this);
  m_seat = std::make_unique<Seat>(*this);
  updateSeatCapabilities();

  m_newOutput.notify = onNewOutput;
  wl_signal_add(&m_backend->events.new_output, &m_newOutput);
  m_newInput.notify = onNewInput;
  wl_signal_add(&m_backend->events.new_input, &m_newInput);

  wlr_log(
      WLR_INFO,
      "mod key: %s (%s session)",
      m_nested ? "Alt" : "Super",
      m_nested ? "nested" : "native");
}

Server::~Server() {
  wl_list_remove(&m_newOutput.link);
  wl_list_remove(&m_newInput.link);
  wl_list_remove(&m_newXdgToplevel.link);
  wl_list_remove(&m_newXdgPopup.link);

  m_views.clear();
  m_keyboards.clear();
  m_outputs.clear();
  m_seat.reset();
  m_cursor.reset();

  wl_display_destroy_clients(m_display);
  wlr_scene_node_destroy(&m_scene->tree.node);
  wlr_allocator_destroy(m_allocator);
  wlr_renderer_destroy(m_renderer);
  wlr_backend_destroy(m_backend);
  wl_display_destroy(m_display);
}

bool Server::start(const char* startupCmd) {
  const char* socket = wl_display_add_socket_auto(m_display);
  if (socket == nullptr) {
    wlr_log(WLR_ERROR, "failed to add Wayland socket");
    return false;
  }
  m_socketName = socket;

  if (!wlr_backend_start(m_backend)) {
    wlr_log(WLR_ERROR, "failed to start backend");
    return false;
  }

  // Point new clients at us. Drop WAYLAND_SOCKET so children do not keep the
  // parent compositor connection (libwayland prefers it over WAYLAND_DISPLAY).
  setenv("WAYLAND_DISPLAY", m_socketName.c_str(), true);
  unsetenv("WAYLAND_SOCKET");
  wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", m_socketName.c_str());

  if (startupCmd != nullptr) {
    spawn(startupCmd);
  }
  return true;
}

void Server::run() { wl_display_run(m_display); }

void Server::stop() { wl_display_terminate(m_display); }

uint32_t Server::modKey() const {
  return m_nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;
}

void Server::focusView(View* view) {
  if (view == nullptr) {
    return;
  }

  auto it = std::find_if(m_views.begin(), m_views.end(), [view](const std::unique_ptr<View>& entry) {
    return entry.get() == view;
  });
  if (it != m_views.end() && it != m_views.begin()) {
    auto entry = std::move(*it);
    m_views.erase(it);
    m_views.insert(m_views.begin(), std::move(entry));
  }

  view->focus();
}

View* Server::viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy) {
  wlr_scene_node* node = wlr_scene_node_at(&m_scene->tree.node, lx, ly, sx, sy);
  if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
    return nullptr;
  }

  wlr_scene_buffer* sceneBuffer = wlr_scene_buffer_from_node(node);
  wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(sceneBuffer);
  if (sceneSurface == nullptr) {
    return nullptr;
  }

  *surface = sceneSurface->surface;
  wlr_scene_tree* tree = node->parent;
  while (tree != nullptr && tree->node.data == nullptr) {
    tree = tree->node.parent;
  }
  return tree != nullptr ? static_cast<View*>(tree->node.data) : nullptr;
}

bool Server::handleKeybind(uint32_t keysym) {
  switch (keysym) {
  case XKB_KEY_Escape:
    stop();
    return true;
  case XKB_KEY_Return: {
    const char* terminal = std::getenv("TERMINAL");
    if (terminal == nullptr || terminal[0] == '\0') {
      wlr_log(WLR_ERROR, "mod+Return: set TERMINAL to your terminal binary");
      return true;
    }
    spawn(terminal);
    return true;
  }
  case XKB_KEY_F1:
    if (m_views.size() >= 2) {
      auto current = std::move(m_views.front());
      m_views.erase(m_views.begin());
      m_views.push_back(std::move(current));
      focusView(m_views.front().get());
    }
    return true;
  default:
    return false;
  }
}

void Server::spawn(const char* command) {
  if (m_socketName.empty()) {
    wlr_log(WLR_ERROR, "cannot spawn before the Wayland socket exists");
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    wlr_log(WLR_ERROR, "fork failed");
    return;
  }
  if (pid == 0) {
    setenv("WAYLAND_DISPLAY", m_socketName.c_str(), 1);
    unsetenv("WAYLAND_SOCKET");
    // Avoid X11/XWayland fallback into the parent session.
    unsetenv("DISPLAY");
    execl("/bin/sh", "/bin/sh", "-c", command, nullptr);
    _exit(1);
  }

  wlr_log(WLR_INFO, "spawned '%s' on WAYLAND_DISPLAY=%s", command, m_socketName.c_str());
}

void Server::updateSeatCapabilities() {
  m_seat->updateCapabilities(!m_keyboards.empty());
}

void Server::onNewOutput(wl_listener* listener, void* data) {
  Server* self = wl_container_of(listener, self, m_newOutput);
  self->addOutput(static_cast<wlr_output*>(data));
}

void Server::onNewInput(wl_listener* listener, void* data) {
  Server* self = wl_container_of(listener, self, m_newInput);
  auto* device = static_cast<wlr_input_device*>(data);
  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    self->addKeyboard(device);
    break;
  case WLR_INPUT_DEVICE_POINTER:
    self->addPointer(device);
    break;
  default:
    break;
  }
  self->updateSeatCapabilities();
}

void Server::onNewXdgToplevel(wl_listener* listener, void* data) {
  Server* self = wl_container_of(listener, self, m_newXdgToplevel);
  self->m_views.push_back(std::make_unique<View>(*self, static_cast<wlr_xdg_toplevel*>(data)));
}

void Server::onNewXdgPopup(wl_listener* /*listener*/, void* data) {
  new Popup(static_cast<wlr_xdg_popup*>(data));
}

void Server::addOutput(wlr_output* output) {
  m_outputs.push_back(std::make_unique<Output>(*this, output));
}

void Server::addKeyboard(wlr_input_device* device) {
  m_keyboards.push_back(std::make_unique<Keyboard>(*this, device));
}

void Server::addPointer(wlr_input_device* device) {
  m_cursor->attachInputDevice(device);
}

void Server::removeOutput(Output* output) {
  std::erase_if(m_outputs, [output](const std::unique_ptr<Output>& entry) {
    return entry.get() == output;
  });
}

void Server::removeKeyboard(Keyboard* keyboard) {
  std::erase_if(m_keyboards, [keyboard](const std::unique_ptr<Keyboard>& entry) {
    return entry.get() == keyboard;
  });
  updateSeatCapabilities();
}

void Server::removeView(View* view) {
  std::erase_if(m_views, [view](const std::unique_ptr<View>& entry) {
    return entry.get() == view;
  });
}

} // namespace umbriel
