#pragma once

#include "core/dirty.h"

#include <cstdint>
#include <memory>
#include <wayland-server-core.h>

struct wlr_gamma_control_v1;
struct wlr_output;
struct wlr_output_layout_output;
struct wlr_scene_output;
struct wlr_scene_optimized_blur;
struct wlr_scene_tree;

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  class Server;
  class WorkspaceGroup;

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
    [[nodiscard]] WorkspaceGroup* workspaceGroup() const { return m_workspaceGroup.get(); }

    void arrangeLayers();
    // Record that something on this output became stale; flushed at the top of
    // the next frame. Schedules that frame, so recording is always enough on its
    // own — marking work that nothing then asks for would simply never happen.
    void markDirty(Dirty what);
    void onGammaChanged(wlr_gamma_control_v1* control);
    void applyConfig();
    void markBlurBackgroundDirty();
    void handleExternalConfigChange();

  private:
    // Flushed at the top of handleFrame, in Dirty declaration order: layer
    // arrange defines the usable area, which the layout depends on, which the
    // chrome over it depends on.
    void flushDirty();

    Dirty m_dirty = Dirty::None;

    static void onFrame(wl_listener* listener, void* data);
    static void onRequestState(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleFrame();
    void handleRequestState(void* data);
    void handleDestroy();
    void applyMode(int width, int height);
    void applyConfiguredState();
    wlr_output_layout_output* addToLayout();
    void arrangeLayer(wlr_scene_tree* tree, const wlr_box* fullArea, wlr_box* usableArea, bool exclusive);
    void updateOptimizedBlur(const wlr_box& fullArea);

    Server* m_server = nullptr;
    wlr_output* m_output = nullptr;
    wlr_scene_output* m_sceneOutput = nullptr;
    wlr_scene_tree* m_layerTrees[kLayerCount]{};
    wlr_scene_tree* m_popupTree = nullptr;
    wlr_scene_optimized_blur* m_optimizedBlur = nullptr;
    std::unique_ptr<WorkspaceGroup> m_workspaceGroup;
    wlr_box m_usableArea{};

    bool m_inFrame = false;
    bool m_hasDeferredMode = false;
    bool m_gammaDirty = false;
    int m_deferredWidth = 0;
    int m_deferredHeight = 0;

    wl_listener m_frame{};
    wl_listener m_requestState{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
