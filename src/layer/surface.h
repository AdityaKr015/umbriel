#pragma once

#include "scene/node.h"

#include <wayland-server-core.h>

struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;
struct wlr_scene_tree;
struct wlr_xdg_popup;

namespace umbriel {

class Output;
class Server;

class LayerSurface : public SceneNode {
public:
  LayerSurface(Server& server, wlr_layer_surface_v1* layerSurface);
  ~LayerSurface();

  LayerSurface(const LayerSurface&) = delete;
  LayerSurface& operator=(const LayerSurface&) = delete;

  [[nodiscard]] wlr_layer_surface_v1* layerSurface() const { return m_layerSurface; }
  [[nodiscard]] wlr_scene_layer_surface_v1* scene() const { return m_scene; }
  [[nodiscard]] bool mapped() const { return m_mapped; }
  [[nodiscard]] bool arrangingOut() const { return m_arrangingOut; }
  [[nodiscard]] bool exclusiveKeyboard() const;
  [[nodiscard]] bool acceptsKeyboard() const;
  [[nodiscard]] bool hasKeyboardFocus() const;

  void focus();
  void unconstrainPopup(wlr_xdg_popup* popup);

private:
  friend class Server;

  static void onMap(wl_listener* listener, void* data);
  static void onUnmap(wl_listener* listener, void* data);
  static void onCommit(wl_listener* listener, void* data);
  static void onDestroy(wl_listener* listener, void* data);
  static void onNewPopup(wl_listener* listener, void* data);

  void handleMap();
  void handleUnmap();
  void handleCommit();
  void handleDestroy();
  void handleNewPopup(void* data);
  void reparentToLayer(uint32_t layer);
  Output* output() const;

  Server* m_server = nullptr;
  wlr_layer_surface_v1* m_layerSurface = nullptr;
  wlr_scene_layer_surface_v1* m_scene = nullptr;
  bool m_mapped = false;
  bool m_arrangingOut = false;

  wl_listener m_map{};
  wl_listener m_unmap{};
  wl_listener m_commit{};
  wl_listener m_destroy{};
  wl_listener m_newPopup{};
};

} // namespace umbriel
