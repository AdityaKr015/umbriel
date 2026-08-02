#pragma once

#include <wayland-server-core.h>

#include <memory>
#include <vector>

struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_output;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output_layout;

namespace umbriel {

class Output;

class Server {
public:
  Server();
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool start();
  void run();
  void stop();

  [[nodiscard]] wl_display* display() const { return m_display; }
  [[nodiscard]] wlr_backend* backend() const { return m_backend; }
  [[nodiscard]] wlr_renderer* renderer() const { return m_renderer; }
  [[nodiscard]] wlr_allocator* allocator() const { return m_allocator; }
  [[nodiscard]] wlr_scene* scene() const { return m_scene; }
  [[nodiscard]] wlr_output_layout* outputLayout() const { return m_outputLayout; }
  [[nodiscard]] wlr_scene_output_layout* sceneLayout() const { return m_sceneLayout; }

private:
  friend class Output;

  static void onNewOutput(wl_listener* listener, void* data);

  void addOutput(wlr_output* output);
  void removeOutput(Output* output);

  wl_display* m_display = nullptr;
  wlr_backend* m_backend = nullptr;
  wlr_renderer* m_renderer = nullptr;
  wlr_allocator* m_allocator = nullptr;
  wlr_compositor* m_compositor = nullptr;
  wlr_output_layout* m_outputLayout = nullptr;
  wlr_scene* m_scene = nullptr;
  wlr_scene_output_layout* m_sceneLayout = nullptr;

  wl_listener m_newOutput{};
  std::vector<std::unique_ptr<Output>> m_outputs;
};

} // namespace umbriel
