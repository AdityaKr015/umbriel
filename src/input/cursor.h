#pragma once
#include "layout/drop_target.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <wayland-server-core.h>

struct wlr_cursor;
struct wlr_input_device;
struct wlr_output;
struct wlr_pointer_constraint_v1;
struct wlr_surface;
struct wlr_tablet;
struct wlr_tablet_tool;
struct wlr_tablet_v2_tablet_tool;
struct wlr_xcursor_manager;

namespace umbriel {

  class LayerSurface;
  class Server;
  class View;
  class Workspace;
  struct ResizeGrab;

  class Cursor {
    struct PassthroughGrab {};
    struct FloatingMoveGrab {
      View* view = nullptr;
      double offsetX = 0;
      double offsetY = 0;
    };
    struct TiledMoveGrab {
      View* view = nullptr;
      double offsetX = 0;
      double offsetY = 0;
      Workspace* sourceWorkspace = nullptr;
      int sourceColumn = -1;
      std::optional<DropColumnWidth> sourceWidth;
      DropTarget drop;
      bool pending = true;
      double startX = 0;
      double startY = 0;
    };
    struct FloatingResizeGrab {
      View* view = nullptr;
      double offsetX = 0;
      double offsetY = 0;
      int geometryX = 0;
      int geometryY = 0;
      int geometryWidth = 0;
      int geometryHeight = 0;
      uint32_t edges = 0;
    };
    struct TiledResizeGrab {
      View* view = nullptr;
      Workspace* workspace = nullptr;
      double startX = 0;
      double startY = 0;
      uint32_t edges = 0;
      std::unique_ptr<ResizeGrab> session;
    };
    using GrabState =
        std::variant<PassthroughGrab, FloatingMoveGrab, TiledMoveGrab, FloatingResizeGrab, TiledResizeGrab>;

    struct TabletToolState {
      Cursor* cursor = nullptr;
      wlr_tablet_tool* tool = nullptr;
      wlr_tablet_v2_tablet_tool* v2 = nullptr;
      // Deliver via wl_pointer instead of tablet-v2.
      bool emulating = false;
      bool tipDown = false;
      // Last absolute position, 0..1.
      double x = 0;
      double y = 0;
      // Last tilt pair.
      double tiltX = 0;
      double tiltY = 0;
      wl_listener destroy{};
      wl_listener setCursor{};
    };

  public:
    explicit Cursor(Server& server);
    ~Cursor();

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    [[nodiscard]] wlr_cursor* wlr() const { return m_cursor; }
    [[nodiscard]] wlr_xcursor_manager* xcursorManager() const { return m_xcursorManager; }
    [[nodiscard]] bool isPassthrough() const;
    [[nodiscard]] View* grabbedView() const;
    // True while `view` is under an interactive move and spans outputs
    // unclipped. Layout code must not re-clip it during the drag.
    [[nodiscard]] bool isDraggingView(const View* view) const;
    // A tiled resize updates every view in its workspace without size
    // animations trailing the pointer.
    [[nodiscard]] bool isResizingWorkspace(const Workspace* workspace) const;

    void attachInputDevice(wlr_input_device* device);
    void applyConfig();
    void setCursorSurface(wlr_surface* surface, int32_t hotspotX, int32_t hotspotY);
    void setXcursor(const char* name);
    void beginMove(View* view);
    void beginResize(View* view, uint32_t edges);
    void resetMode();
    // Warp the cursor to a layout position and run the motion pipeline (pointer
    // focus, xcursor, pointer-output tracking, idle). Output actions use it so
    // focus follows the cursor onto the target monitor.
    void warpTo(double lx, double ly);
    // A layout-mode reload can replace the layout that owns a tiled resize.
    void cancelStaleTiledResize();
    void handleNewConstraint(wlr_pointer_constraint_v1* constraint);
    void clearConstraint();
    // Recompute compositor cursor (mod-held resize/move affordance, or active grab).
    void refreshInteractiveCursor();
    // Compositor-owned cursor override for grabs the Cursor does not track
    // (overview drag). nullptr restores the client cursor.
    void overrideCursor(const char* name) { setCompositorCursor(name); }
    [[nodiscard]] bool compositorOwnsCursor() const { return m_compositorOwnsCursor; }

  private:
    static void onMotion(wl_listener* listener, void* data);
    static void onMotionAbsolute(wl_listener* listener, void* data);
    static void onButton(wl_listener* listener, void* data);
    static void onAxis(wl_listener* listener, void* data);
    static void onFrame(wl_listener* listener, void* data);
    static void onConstraintDestroy(wl_listener* listener, void* data);
    static void onTouchDown(wl_listener* listener, void* data);
    static void onTouchUp(wl_listener* listener, void* data);
    static void onTouchMotion(wl_listener* listener, void* data);
    static void onTouchCancel(wl_listener* listener, void* data);
    static void onTouchFrame(wl_listener* listener, void* data);
    static void onTabletToolAxis(wl_listener* listener, void* data);
    static void onTabletToolProximity(wl_listener* listener, void* data);
    static void onTabletToolTip(wl_listener* listener, void* data);
    static void onTabletToolButton(wl_listener* listener, void* data);
    static void onToolDestroy(wl_listener* listener, void* data);
    static void onToolSetCursor(wl_listener* listener, void* data);

    void handleMotion(void* data);
    void handleMotionAbsolute(void* data);
    void handleButton(void* data);
    void handleAxis(void* data);
    void handleFrame();
    void handleConstraintDestroy();
    void handleTouchDown(void* data);
    void handleTouchUp(void* data);
    void handleTouchMotion(void* data);
    void handleTouchCancel(void* data);
    void handleTouchFrame();
    void handleTabletToolAxis(void* data);
    void handleTabletToolProximity(void* data);
    void handleTabletToolTip(void* data);
    void handleTabletToolButton(void* data);

    void processMotion(uint32_t timeMsec, double oldX, double oldY);
    void processButton(uint32_t timeMsec, uint32_t button, wl_pointer_button_state state);
    void updatePointerOutput();
    View* hoverFocus(
        View* view, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer, double oldX, double oldY
    );
    void processMove();
    void presentGrabbedViewSpanning();
    void updateDropTarget();
    void finishTileMove();
    void finishFloatMove();
    void processResize();
    void processResizeTile();
    [[nodiscard]] uint32_t floatResizeEdges(View* view) const;
    [[nodiscard]] uint32_t hoverResizeEdges(View* view) const;
    void updateInteractiveCursor(View* under);
    void setCompositorCursor(const char* name);
    void restoreClientCursor();
    void setActiveConstraint(wlr_pointer_constraint_v1* constraint);
    void updateConstraintForSurface(wlr_surface* surface);
    [[nodiscard]] bool constraintSurfaceActive() const;
    void warpToConstraintHint(wlr_pointer_constraint_v1* constraint);
    [[nodiscard]] bool confineDelta(double* dx, double* dy) const;
    TabletToolState* toolState(wlr_tablet_tool* tool);
    void setToolEmulating(TabletToolState* state, bool emulating);
    void processTabletMotion(uint32_t timeMsec, double oldX, double oldY, TabletToolState* state, wlr_tablet* tablet);

    Server* m_server = nullptr;
    wlr_cursor* m_cursor = nullptr;
    wlr_xcursor_manager* m_xcursorManager = nullptr;
    wlr_xcursor_manager* m_activeXcursorManager = nullptr;
    std::string m_xcursorTheme;
    std::string m_activeXcursorName;
    int m_xcursorSize = 0;
    wlr_pointer_constraint_v1* m_activeConstraint = nullptr;

    GrabState m_grab;
    // Last layout output under the pointer; crossing heads updates seat focus like workspace switch.
    wlr_output* m_pointerOutput = nullptr;
    double m_wheelAccum[2]{};
    // Presses consumed by config mouse binds; their release is swallowed too.
    std::vector<uint32_t> m_swallowedButtons;
    std::vector<std::unique_ptr<TabletToolState>> m_tools;
    bool m_compositorOwnsCursor = false;
    std::string m_compositorCursorName;

    wl_listener m_motion{};
    wl_listener m_motionAbsolute{};
    wl_listener m_button{};
    wl_listener m_axis{};
    wl_listener m_frame{};
    wl_listener m_constraintDestroy{};
    wl_listener m_touchDown{};
    wl_listener m_touchUp{};
    wl_listener m_touchMotion{};
    wl_listener m_touchCancel{};
    wl_listener m_touchFrame{};
    wl_listener m_tabletToolAxis{};
    wl_listener m_tabletToolProximity{};
    wl_listener m_tabletToolTip{};
    wl_listener m_tabletToolButton{};
  };

} // namespace umbriel
