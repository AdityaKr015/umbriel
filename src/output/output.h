#pragma once

#include <wayland-server-core.h>

struct wlr_output;
struct wlr_scene_output;

namespace umbriel {

class Server;

class Output {
public:
  Output(Server& server, wlr_output* output);
  ~Output();

  Output(const Output&) = delete;
  Output& operator=(const Output&) = delete;

  [[nodiscard]] wlr_output* wlr() const { return m_output; }

private:
  static void onFrame(wl_listener* listener, void* data);
  static void onRequestState(wl_listener* listener, void* data);
  static void onDestroy(wl_listener* listener, void* data);

  void handleFrame();
  void handleRequestState(void* data);
  void handleDestroy();
  void applyMode(int width, int height);

  Server* m_server = nullptr;
  wlr_output* m_output = nullptr;
  wlr_scene_output* m_sceneOutput = nullptr;

  bool m_inFrame = false;
  bool m_hasDeferredMode = false;
  int m_deferredWidth = 0;
  int m_deferredHeight = 0;

  wl_listener m_frame{};
  wl_listener m_requestState{};
  wl_listener m_destroy{};
};

} // namespace umbriel
