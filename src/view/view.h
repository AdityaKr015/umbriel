#pragma once
#include "core/animation.h"
#include "scene/node.h"
#include "scene/surface_blur.h"
#include "scene/surface_shadow.h"

#include <array>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/util/box.h>
}
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_image_capture_source_v1;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_output;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_xdg_popup;
struct wlr_xdg_toplevel;

namespace umbriel {

  class Server;
  class Workspace;

  class View : public SceneNode {
  public:
    View(Server& server, wlr_xdg_toplevel* toplevel);
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    [[nodiscard]] wlr_xdg_toplevel* toplevel() const { return m_toplevel; }
    [[nodiscard]] wlr_scene_tree* sceneTree() const { return m_sceneTree; }
    [[nodiscard]] bool mapped() const { return m_mapped; }
    [[nodiscard]] Workspace* workspace() const { return m_workspace; }
    [[nodiscard]] bool onActiveWorkspace() const { return m_onActiveWorkspace; }
    [[nodiscard]] bool tiled() const { return m_tiled; }
    [[nodiscard]] bool floating() const { return !m_tiled; }
    [[nodiscard]] bool sizeAnimActive() const { return m_sizeAnim != 0; }
    [[nodiscard]] int presentedWidth(const wlr_box& target) const;
    [[nodiscard]] int presentedHeight(const wlr_box& target) const;
    [[nodiscard]] wlr_scene_tree* homeTree() const;

    // Mechanism only — applies seat keyboard, activation chrome, and raise.
    // Policy lives in Server::focusView; do not call from input/event code.
    void applySeatFocus();
    void setForeignActivated(bool activated);
    void setWorkspace(Workspace* workspace, bool attachToLayout = true);
    void detachWorkspace();
    void setOnActiveWorkspace(bool active);
    void animateTo(int x, int y);
    void setPosition(int x, int y);
    void setOutputClip(const wlr_box* screenIntersection, const wlr_box& target, const wlr_box& outputBox);
    void setFadeAlpha(float alpha);
    void cancelFadeAnimation();
    void cancelPositionAnimation();
    // Size/position to the full output and drop tile clips (exclusive zones do not apply).
    void applyFullscreenLayout();
    // Compositor-driven fullscreen toggle (keybind); client requests use handleRequestFullscreen.
    void toggleFullscreen();
    // Detach from the scrolling layout (float) or re-insert as a tiled column.
    void setFloating(bool floating);
    void toggleFloating();
    // Enable/disable the view's scene tree and its shadow container together.
    void setNodeEnabled(bool enabled);
    // Create or destroy the shadow container in the given workspace shadow layer.
    void reparentShadow(wlr_scene_tree* shadowLayer);

  private:
    friend class Cursor;
    friend class Server;
    friend class Popup;

    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onCommit(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onRequestMove(wl_listener* listener, void* data);
    static void onRequestResize(wl_listener* listener, void* data);
    static void onRequestMaximize(wl_listener* listener, void* data);
    static void onRequestFullscreen(wl_listener* listener, void* data);
    static void onSetTitle(wl_listener* listener, void* data);
    static void onSetAppId(wl_listener* listener, void* data);
    static void onForeignActivate(wl_listener* listener, void* data);
    static void onForeignClose(wl_listener* listener, void* data);
    static void onForeignDestroy(wl_listener* listener, void* data);
    static void onExtForeignDestroy(wl_listener* listener, void* data);

    static void onCaptureSourceDestroy(wl_listener* listener, void* data);
    void handleMap();
    void handleUnmap();
    void handleCommit();
    void handleDestroy();
    void handleRequestMove();
    void handleRequestResize(void* data);
    void handleRequestMaximize();
    void handleRequestFullscreen();
    void setFullscreen(bool fullscreen);
    void handleSetTitle();
    void handleSetAppId();
    void handleForeignActivate();
    void handleForeignClose();
    void handleForeignDestroy();
    void handleExtForeignDestroy();
    struct BorderEdge;
    void handleCaptureSourceDestroy();
    static std::array<BorderEdge, 4> makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness);
    [[nodiscard]] std::array<BorderEdge, 4> borderEdges() const;
    [[nodiscard]] std::array<BorderEdge, 4> borderEdges(int contentWidth, int contentHeight) const;
    void updateBorderGeometry();
    void updateBorderGeometry(int contentWidth, int contentHeight);
    void setBorderFocused(bool focused);
    void applyBorderClip(
        wlr_scene_rect* const rects[4], const std::array<BorderEdge, 4>& edges, const wlr_box& target,
        const wlr_box& outputBox
    );
    void applyOuterBorderClip(const wlr_box& target, const wlr_box& outputBox, int contentWidth, int contentHeight);
    void applyCornerRadius();
    void updateBlur();
    void updateShadow();
    void beginCloseAnimation();
    void applyPresentedSize();
    void cancelSizeAnimation();
    void updateFullscreenPresentation(int width, int height);
    void clearOutputClip();
    // Apply subsurface clip to the toplevel surface only, not xdg popup children.
    void setSurfaceTreeClip(const wlr_box* clip);
    void unconstrainPopup(wlr_xdg_popup* popup);
    // Keep floats visually at the last requested size while client geometry lags.
    void syncFloatingSurfaceClip();
    void placeInUsableArea();
    void ensureBorders();
    void updateForeignIdentity();
    void updateForeignState();
    void enterForeignOutput();
    void leaveForeignOutput();
    void applyWindowRules(bool allowDisruptive);
    void applyRuleOpacity();

    Server* m_server = nullptr;
    wlr_xdg_toplevel* m_toplevel = nullptr;
    wlr_scene_tree* m_sceneTree = nullptr;
    wlr_scene_tree* m_borderTree = nullptr;
    wlr_scene_rect* m_borderRects[4] = {};       // top, bottom, left, right (inner)
    wlr_scene_rect* m_outerBorderRect = nullptr; // single rounded ring outside the inner border
    wlr_scene_rect* m_fullscreenBackdrop = nullptr;
    SurfaceBlur m_blur;
    SurfaceShadow m_shadow;
    wlr_scene_tree* m_shadowContainer = nullptr; // child of workspace shadow layer
    bool m_hasShadowOutputClip = false;
    wlr_box m_shadowOutputClip{}; // global (scene-root), valid when m_hasShadowOutputClip
    wlr_foreign_toplevel_handle_v1* m_foreign = nullptr;
    wlr_ext_foreign_toplevel_handle_v1* m_extForeign = nullptr;
    wlr_output* m_foreignOutput = nullptr;
    wlr_ext_image_capture_source_v1* m_captureSource = nullptr;
    Workspace* m_workspace = nullptr;
    bool m_mapped = false;
    bool m_tiled = false;
    bool m_onActiveWorkspace = false;
    AnimId m_posAnim = 0;
    AnimId m_fadeAnim = 0;
    float m_fadeAlpha = 1.0F;
    bool m_borderFocusedState = false;
    AnimId m_sizeAnim = 0;
    int m_presentedW = 0;
    int m_presentedH = 0;
    int m_sizeTargetW = 0;
    int m_sizeTargetH = 0;
    // Fullscreen: content offset centering a stale (smaller) buffer in the tile.
    int m_fullscreenOffsetX = 0;
    int m_fullscreenOffsetY = 0;
    bool m_fullscreenContentCentered = false;
    // Window rules: unsettled means title was empty at map, so a later
    // handleSetTitle re-applies all rule effects one more time.
    bool m_initialRulesSettled = false;
    float m_ruleOpacity = 1.0F;

    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_commit{};
    wl_listener m_destroy{};
    wl_listener m_requestMove{};
    wl_listener m_requestResize{};
    wl_listener m_requestMaximize{};
    wl_listener m_requestFullscreen{};
    wl_listener m_setTitle{};
    wl_listener m_setAppId{};
    wl_listener m_foreignActivate{};
    wl_listener m_foreignClose{};
    wl_listener m_foreignDestroy{};
    wl_listener m_extForeignDestroy{};
    wl_listener m_captureSourceDestroy{};
  };

} // namespace umbriel
