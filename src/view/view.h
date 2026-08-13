#pragma once
#include "core/animation.h"
#include "scene/node.h"
#include "scene/surface_blur.h"
#include "scene/surface_shadow.h"

#include <array>
#include <optional>
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
  struct ResolvedWindowRule;

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
    [[nodiscard]] bool pinned() const { return m_pinned; }
    [[nodiscard]] bool sizeAnimActive() const { return sizeAnimating(); }
    [[nodiscard]] int presentedWidth(const wlr_box& target) const;
    [[nodiscard]] int presentedHeight(const wlr_box& target) const;
    [[nodiscard]] wlr_scene_tree* homeTree() const;

    // Mechanism only — applies seat keyboard, activation chrome, and raise.
    // Policy lives in Server::focusView; do not call from input/event code.
    // `withKeyboard` is false while overview owns the seat: chrome and
    // activation still update, the keyboard enter is deferred to the close.
    void applySeatFocus(bool withKeyboard = true);
    void setForeignActivated(bool activated);
    void setWorkspace(Workspace* workspace, bool attachToLayout = true);
    void detachWorkspace();
    void setOnActiveWorkspace(bool active);
    void setScratchpadBorder(bool scratchpad);
    void animateTo(int x, int y);
    void setPosition(int x, int y);
    // Keep at least clamp(size / 4, 10, 75) pixels per axis on-screen.
    void clampFloatingPosition();
    // Animate the presented size toward a layout-assigned size. Called by
    // Workspace::arrange when it configures the client, so the animation owns
    // m_presentedW/H before the clip can report the final size.
    void beginResizeAnimation(int width, int height);
    void setOutputClip(const wlr_box* screenIntersection, const wlr_box& target, const wlr_box& outputBox);
    // Drop the per-output clip so the view renders unclipped (e.g. a window
    // dragged across a monitor boundary spans both outputs).
    void clearOutputClip();
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
    void togglePinned();
    // Restore the global pinned scene layer after temporary drag reparenting.
    void restorePinnedSceneParent();
    // Enable/disable the view's scene tree and its shadow container together.
    void setNodeEnabled(bool enabled);
    void raiseToTop();
    // Create or destroy the shadow container in the given workspace shadow layer.
    void reparentShadow(wlr_scene_tree* shadowLayer);
    // Advances this view's animations; returns true while any is still running.
    bool tickAnimations(uint64_t nowMsec);
    [[nodiscard]] bool hasActiveAnimations() const;

  private:
    friend class Cursor;
    friend class Server;
    friend class Popup;
    friend class Overview;

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
    void setMaximized(bool maximized);
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
    [[nodiscard]] SurfaceBlurOptions blurOptions() const;
    [[nodiscard]] SurfaceBlurOptions popupBlurOptions() const;
    void updateShadow();
    // Record the dimensions currently rendered by the scene. Client geometry
    // can lag a layout configure, so presentation consumers must not infer
    // their size independently from the committed geometry.
    void trackPresentedSize(int width, int height);
    // Re-apply m_fadeAlpha*m_ruleOpacity to surface buffers. wlroots' scene
    // surface reconfigure (on commit or clip change) resets buffer opacity, so
    // this must run after any such operation while effective opacity is < 1.
    void applyEffectiveOpacity();
    void beginCloseAnimation();
    void applyPresentedSize();
    // Scale-then-crop presentation of the primary buffer during a size
    // animation; surfaceClip is the visible presented region in surface coords.
    void applyPresentedCrop(const wlr_box& content, const wlr_box& surfaceClip);
    // Undo applyPresentedCrop/size-anim buffer state when the animation ends.
    void resetPresentedSurface();
    // Shared tail of a finished/cancelled size animation: settle the presented
    // size on the committed geometry and refresh the derived chrome.
    void finishSizeAnimation();
    [[nodiscard]] bool sizeAnimating() const { return m_animW.animating() || m_animH.animating(); }
    // True while an interactive grab owns this view's size, so the layout must
    // not animate it (the drag tracks the pointer 1:1).
    [[nodiscard]] bool sizeGrabActive() const;
    // Kick the owning output so an animation started outside a frame gets ticked.
    void scheduleFrame();
    void cancelSizeAnimation();
    void updateFullscreenPresentation(int width, int height);
    // Apply subsurface clip to the toplevel surface only, not xdg popup children.
    void setSurfaceTreeClip(const wlr_box* clip);
    void unconstrainPopup(wlr_xdg_popup* popup);
    // Keep floats visually at the last requested size while client geometry lags.
    void syncFloatingSurfaceClip();
    void requestFloatingSize(int width, int height);
    void beginFloatingResize(uint32_t edges);
    void resizeFloating(int width, int height);
    void finishFloatingResize();
    void syncFloatingResizePosition();
    void adoptFloatingClientSize();
    [[nodiscard]] wlr_box floatingUsableArea() const;
    void placeInUsableArea();
    void ensureBorders();
    void updateForeignIdentity();
    void updateForeignState();
    void enterForeignOutput();
    void leaveForeignOutput();
    void applyWindowRules(bool allowDisruptive);
    // `resolved` lets a caller that already resolved the rules pass them in.
    // Rule resolution runs every regex in the config, and applyWindowRules is
    // reached on focus changes and on every title change, so resolving twice per
    // pass is work a terminal that retitles per command pays repeatedly.
    void applyDynamicRules(const ResolvedWindowRule* resolved = nullptr);

    Server* m_server = nullptr;
    wlr_xdg_toplevel* m_toplevel = nullptr;
    wlr_scene_tree* m_sceneTree = nullptr;
    wlr_scene_tree* m_borderTree = nullptr;
    wlr_scene_rect* m_borderRects[4] = {};       // top, bottom, left, right (inner)
    wlr_scene_rect* m_outerBorderRect = nullptr; // single rounded ring outside the inner border
    wlr_scene_rect* m_fullscreenBackdrop = nullptr;
    SurfaceBlur m_blur;
    SurfaceBlurOptions m_blurOptions;
    SurfaceBlurOptions m_popupBlurOptions;
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
    // False until the first setPosition/animateTo places the node; the initial
    // placement snaps (avoids animating from the default (0,0) world origin).
    bool m_positioned = false;
    bool m_tiled = false;
    bool m_pinned = false;
    bool m_onActiveWorkspace = false;
    bool m_scratchpadBorder = false;
    AnimatedValue m_posX;
    AnimatedValue m_posY;
    AnimatedValue m_fade;
    float m_fadeAlpha = 1.0F;
    bool m_borderFocusedState = false;
    AnimatedValue m_animW;
    AnimatedValue m_animH;
    // Dimensions currently rendered by the scene. These follow the layout clip
    // while client geometry lags, and the interpolated size during animation.
    int m_presentedW = 0;
    int m_presentedH = 0;
    // Fullscreen: content offset centering a stale (smaller) buffer in the tile.
    int m_fullscreenOffsetX = 0;
    int m_fullscreenOffsetY = 0;
    bool m_fullscreenContentCentered = false;
    // Window rules: unsettled means title was empty at map, so a later
    // handleSetTitle re-applies all rule effects one more time.
    bool m_initialRulesSettled = false;
    float m_ruleOpacity = 1.0F;
    bool m_hasMaximizeRestoreBox = false;
    wlr_box m_maximizeRestoreBox{};
    // Floating geometry memory survives tiled-to-floating round trips.
    std::optional<std::array<int, 2>> m_floatingSize;
    std::optional<std::array<double, 2>> m_floatingPosFrac;
    // Latest compositor-owned floating size request. Client geometry becomes
    // authoritative after committing this configure serial.
    std::optional<uint32_t> m_floatingSizeRequestSerial;
    // Content box at interactive-resize start. Left/top resizes keep the
    // opposite edge fixed while client geometry catches up asynchronously.
    std::optional<wlr_box> m_floatingResizeAnchor;
    uint32_t m_floatingResizeEdges = 0;
    bool m_floatingResizeActive = false;

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
