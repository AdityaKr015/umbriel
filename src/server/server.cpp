#include "server/server.h"

#include "core/log.h"
#include "input/cursor.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "output/output.h"
#include "scene/node.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace umbriel {

namespace {

constexpr Logger kLog("server");

} // namespace

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

  // Windows sit between per-output bottom and top layer trees.
  m_xdgTree = wlr_scene_tree_create(&m_scene->tree);

  m_xdgShell = wlr_xdg_shell_create(m_display, 3);
  m_newXdgToplevel.notify = onNewXdgToplevel;
  wl_signal_add(&m_xdgShell->events.new_toplevel, &m_newXdgToplevel);
  m_newXdgPopup.notify = onNewXdgPopup;
  wl_signal_add(&m_xdgShell->events.new_popup, &m_newXdgPopup);

  m_layerShell = wlr_layer_shell_v1_create(m_display, 4);
  m_newLayerSurface.notify = onNewLayerSurface;
  wl_signal_add(&m_layerShell->events.new_surface, &m_newLayerSurface);

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
  kLog.info("mod key: {} ({} session)", m_nested ? "Alt" : "Super", m_nested ? "nested" : "native");
}

Server::~Server() {
  wl_list_remove(&m_newOutput.link);
  wl_list_remove(&m_newInput.link);
  wl_list_remove(&m_newXdgToplevel.link);
  wl_list_remove(&m_newXdgPopup.link);
  wl_list_remove(&m_newLayerSurface.link);

  m_layerSurfaces.clear();
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
  kLog.info("running on WAYLAND_DISPLAY={}", m_socketName);
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

View* Server::viewAt(
    double lx, double ly, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer) {
  if (layer != nullptr) {
    *layer = nullptr;
  }

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
  if (tree == nullptr) {
    return nullptr;
  }

  auto* sceneNode = static_cast<SceneNode*>(tree->node.data);
  if (sceneNode->kind == SceneNodeKind::LayerSurface) {
    if (layer != nullptr) {
      *layer = static_cast<LayerSurface*>(sceneNode);
    }
    return nullptr;
  }
  return static_cast<View*>(sceneNode);
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

void Server::arrangeLayers(wlr_output* output) {
  if (Output* out = outputFromWlr(output)) {
    out->arrangeLayers();
  }
}

wlr_output* Server::preferredOutput() const {
  wlr_output* output =
      wlr_output_layout_output_at(m_outputLayout, m_cursor->wlr()->x, m_cursor->wlr()->y);
  if (output != nullptr) {
    return output;
  }
  if (!m_outputs.empty()) {
    return m_outputs.front()->wlr();
  }
  return nullptr;
}

Output* Server::outputFromWlr(wlr_output* output) const {
  if (output == nullptr) {
    return nullptr;
  }
  if (output->data != nullptr) {
    return static_cast<Output*>(output->data);
  }
  for (const auto& entry : m_outputs) {
    if (entry->wlr() == output) {
      return entry.get();
    }
  }
  return nullptr;
}

wlr_box Server::usableAreaAt(double lx, double ly) const {
  wlr_output* output = wlr_output_layout_output_at(m_outputLayout, lx, ly);
  if (output == nullptr) {
    output = preferredOutput();
  }
  if (Output* out = outputFromWlr(output)) {
    wlr_box usable = out->usableArea();
    if (usable.width > 0 && usable.height > 0) {
      return usable;
    }
  }

  wlr_box fullArea{};
  wlr_output_layout_get_box(m_outputLayout, output, &fullArea);
  return fullArea;
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
  auto* popup = static_cast<wlr_xdg_popup*>(data);
  // Layer-shell popups (and any popup without a parent yet) are handled
  // elsewhere; parent can be null when xdg_shell emits new_popup.
  if (popup->parent == nullptr
      || wlr_xdg_surface_try_from_wlr_surface(popup->parent) == nullptr) {
    return;
  }
  new Popup(popup);
}

void Server::onNewLayerSurface(wl_listener* listener, void* data) {
  Server* self = wl_container_of(listener, self, m_newLayerSurface);
  auto surface = std::make_unique<LayerSurface>(*self, static_cast<wlr_layer_surface_v1*>(data));
  if (surface->layerSurface() == nullptr) {
    return;
  }
  self->m_layerSurfaces.push_back(std::move(surface));
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

void Server::removeLayerSurface(LayerSurface* layerSurface, wlr_output* output) {
  std::erase_if(m_layerSurfaces, [layerSurface](const std::unique_ptr<LayerSurface>& entry) {
    return entry.get() == layerSurface;
  });
  arrangeLayers(output);
}

} // namespace umbriel
