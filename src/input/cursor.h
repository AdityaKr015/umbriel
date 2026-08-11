#pragma once
#include "layout/drop_target.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <wayland-server-core.h>

struct wlr_cursor;
struct wlr_input_device;
struct wlr_output;
struct wlr_pointer_constraint_v1;
struct wlr_surface;
struct wlr_xcursor_manager;

namespace umbriel {

  class Server;
  class View;
  class Workspace;
  struct ResizeGrab;

  enum class CursorMode {
    Passthrough,
    Move,
    MoveTile,
    Resize,
    ResizeTile,
  };

  class Cursor {
  public:
    explicit Cursor(Server& server);
    ~Cursor();

    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    [[nodiscard]] wlr_cursor* wlr() const { return m_cursor; }
    [[nodiscard]] wlr_xcursor_manager* xcursorManager() const { return m_xcursorManager; }
    [[nodiscard]] CursorMode mode() const { return m_mode; }
    [[nodiscard]] View* grabbedView() const { return m_grabbedView; }
    // True while `view` is the window under an interactive move (spans outputs
    // unclipped); layout code must not re-clip it during the drag.
    [[nodiscard]] bool isDraggingView(const View* view) const {
      return view != nullptr && view == m_grabbedView && (m_mode == CursorMode::Move || m_mode == CursorMode::MoveTile);
    }

    void attachInputDevice(wlr_input_device* device);
    void applyConfig();
    void beginInteractive(View* view, CursorMode mode, uint32_t edges);
    void resetMode();
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

    void processMotion(uint32_t timeMsec, double oldX, double oldY);
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

    Server* m_server = nullptr;
    wlr_cursor* m_cursor = nullptr;
    wlr_xcursor_manager* m_xcursorManager = nullptr;
    wlr_pointer_constraint_v1* m_activeConstraint = nullptr;

    CursorMode m_mode = CursorMode::Passthrough;
    View* m_grabbedView = nullptr;
    double m_grabX = 0;
    double m_grabY = 0;
    int m_grabGeoX = 0;
    int m_grabGeoY = 0;
    int m_grabGeoWidth = 0;
    int m_grabGeoHeight = 0;
    uint32_t m_resizeEdges = 0;
    Workspace* m_dragSourceWorkspace = nullptr;
    int m_dragSourceColumn = -1;
    DropTarget m_drop{};
    bool m_tileDragPending = false;
    double m_tileDragStartX = 0;
    double m_tileDragStartY = 0;
    Workspace* m_resizeWorkspace = nullptr;
    double m_resizeStartX = 0;
    double m_resizeStartY = 0;
    // Layout-owned resize session for the active tiled resize (scrolling/dwindle).
    std::unique_ptr<ResizeGrab> m_resizeGrab;
    // Last layout output under the pointer; crossing heads updates seat focus like workspace switch.
    wlr_output* m_pointerOutput = nullptr;
    double m_wheelAccum[2]{};
    // Presses consumed by config mouse binds; their release is swallowed too.
    std::vector<uint32_t> m_swallowedButtons;
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
  };

} // namespace umbriel
