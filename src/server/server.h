#pragma once

#include <wayland-server-core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_input_device;
struct wlr_output;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_surface;
struct wlr_xdg_shell;

namespace umbriel {

class Cursor;
class Keyboard;
class Output;
class Seat;
class View;

class Server {
public:
  Server();
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool start(const char* startupCmd = nullptr);
  void run();
  void stop();

  [[nodiscard]] wl_display* display() const { return m_display; }
  [[nodiscard]] wlr_backend* backend() const { return m_backend; }
  [[nodiscard]] wlr_renderer* renderer() const { return m_renderer; }
  [[nodiscard]] wlr_allocator* allocator() const { return m_allocator; }
  [[nodiscard]] wlr_scene* scene() const { return m_scene; }
  [[nodiscard]] wlr_output_layout* outputLayout() const { return m_outputLayout; }
  [[nodiscard]] wlr_scene_output_layout* sceneLayout() const { return m_sceneLayout; }
  [[nodiscard]] Seat* seat() const { return m_seat.get(); }
  [[nodiscard]] Cursor* cursor() const { return m_cursor.get(); }
  [[nodiscard]] bool nested() const { return m_nested; }
  [[nodiscard]] uint32_t modKey() const;

  void focusView(View* view);
  View* viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy);
  bool handleKeybind(uint32_t keysym);

  void removeOutput(Output* output);
  void removeKeyboard(Keyboard* keyboard);
  void removeView(View* view);

private:
  friend class Output;
  friend class Keyboard;
  friend class Cursor;
  friend class View;
  friend class Seat;

  static void onNewOutput(wl_listener* listener, void* data);
  static void onNewInput(wl_listener* listener, void* data);
  static void onNewXdgToplevel(wl_listener* listener, void* data);
  static void onNewXdgPopup(wl_listener* listener, void* data);

  void addOutput(wlr_output* output);
  void addKeyboard(wlr_input_device* device);
  void addPointer(wlr_input_device* device);
  void updateSeatCapabilities();
  void spawn(const char* command);

  wl_display* m_display = nullptr;
  wlr_backend* m_backend = nullptr;
  wlr_renderer* m_renderer = nullptr;
  wlr_allocator* m_allocator = nullptr;
  wlr_compositor* m_compositor = nullptr;
  wlr_output_layout* m_outputLayout = nullptr;
  wlr_scene* m_scene = nullptr;
  wlr_scene_output_layout* m_sceneLayout = nullptr;
  wlr_xdg_shell* m_xdgShell = nullptr;

  std::unique_ptr<Seat> m_seat;
  std::unique_ptr<Cursor> m_cursor;

  bool m_nested = false;
  std::string m_socketName;

  wl_listener m_newOutput{};
  wl_listener m_newInput{};
  wl_listener m_newXdgToplevel{};
  wl_listener m_newXdgPopup{};

  std::vector<std::unique_ptr<Output>> m_outputs;
  std::vector<std::unique_ptr<Keyboard>> m_keyboards;
  std::vector<std::unique_ptr<View>> m_views;
};

} // namespace umbriel
