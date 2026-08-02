#pragma once

#include "scene/node.h"

#include <wayland-server-core.h>

struct wlr_scene_tree;
struct wlr_xdg_toplevel;

namespace umbriel {

class Server;

class View : public SceneNode {
public:
  View(Server& server, wlr_xdg_toplevel* toplevel);
  ~View();

  View(const View&) = delete;
  View& operator=(const View&) = delete;

  [[nodiscard]] wlr_xdg_toplevel* toplevel() const { return m_toplevel; }
  [[nodiscard]] wlr_scene_tree* sceneTree() const { return m_sceneTree; }
  [[nodiscard]] bool mapped() const { return m_mapped; }

  void focus();

private:
  friend class Cursor;

  static void onMap(wl_listener* listener, void* data);
  static void onUnmap(wl_listener* listener, void* data);
  static void onCommit(wl_listener* listener, void* data);
  static void onDestroy(wl_listener* listener, void* data);
  static void onRequestMove(wl_listener* listener, void* data);
  static void onRequestResize(wl_listener* listener, void* data);
  static void onRequestMaximize(wl_listener* listener, void* data);
  static void onRequestFullscreen(wl_listener* listener, void* data);

  void handleMap();
  void handleUnmap();
  void handleCommit();
  void handleDestroy();
  void handleRequestMove();
  void handleRequestResize(void* data);
  void handleRequestMaximize();
  void handleRequestFullscreen();
  void placeInUsableArea();

  Server* m_server = nullptr;
  wlr_xdg_toplevel* m_toplevel = nullptr;
  wlr_scene_tree* m_sceneTree = nullptr;
  bool m_mapped = false;

  wl_listener m_map{};
  wl_listener m_unmap{};
  wl_listener m_commit{};
  wl_listener m_destroy{};
  wl_listener m_requestMove{};
  wl_listener m_requestResize{};
  wl_listener m_requestMaximize{};
  wl_listener m_requestFullscreen{};
};

} // namespace umbriel
