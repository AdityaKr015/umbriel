#pragma once

#include <wayland-server-core.h>

struct wlr_xdg_popup;
struct wlr_scene_tree;

namespace umbriel {

class Popup {
public:
  explicit Popup(wlr_xdg_popup* popup, wlr_scene_tree* parentTree = nullptr);
  ~Popup();

  Popup(const Popup&) = delete;
  Popup& operator=(const Popup&) = delete;

private:
  static void onCommit(wl_listener* listener, void* data);
  static void onDestroy(wl_listener* listener, void* data);

  void handleCommit();
  void handleDestroy();

  wlr_xdg_popup* m_popup = nullptr;

  wl_listener m_commit{};
  wl_listener m_destroy{};
};

} // namespace umbriel
