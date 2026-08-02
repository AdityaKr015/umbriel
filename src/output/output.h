#pragma once

#include <cstdint>
#include <wayland-server-core.h>

struct wlr_output;
struct wlr_scene_output;
struct wlr_scene_tree;

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  class Server;

  class Output {
  public:
    static constexpr uint32_t kLayerCount = 4;

    Output(Server& server, wlr_output* output);
    ~Output();

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    [[nodiscard]] wlr_output* wlr() const { return m_output; }
    [[nodiscard]] wlr_scene_output* sceneOutput() const { return m_sceneOutput; }
    [[nodiscard]] wlr_scene_tree* layerTree(uint32_t layer) const;
    [[nodiscard]] wlr_scene_tree* popupTree() const { return m_popupTree; }
    [[nodiscard]] wlr_box usableArea() const { return m_usableArea; }

    void arrangeLayers();

  private:
    static void onFrame(wl_listener* listener, void* data);
    static void onRequestState(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleFrame();
    void handleRequestState(void* data);
    void handleDestroy();
    void applyMode(int width, int height);
    void arrangeLayer(wlr_scene_tree* tree, const wlr_box* fullArea, wlr_box* usableArea, bool exclusive);
    void fixSceneOrder();

    Server* m_server = nullptr;
    wlr_output* m_output = nullptr;
    wlr_scene_output* m_sceneOutput = nullptr;
    wlr_scene_tree* m_layerTrees[kLayerCount]{};
    wlr_scene_tree* m_popupTree = nullptr;
    wlr_box m_usableArea{};

    bool m_inFrame = false;
    bool m_hasDeferredMode = false;
    int m_deferredWidth = 0;
    int m_deferredHeight = 0;

    wl_listener m_frame{};
    wl_listener m_requestState{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
