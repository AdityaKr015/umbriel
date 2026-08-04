#pragma once

#include <cstdint>
#include <string>
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

    void attachInputDevice(wlr_input_device* device);
    void applyConfig();
    void beginInteractive(View* view, CursorMode mode, uint32_t edges);
    void resetMode();
    void handleNewConstraint(wlr_pointer_constraint_v1* constraint);
    void clearConstraint();
    // Recompute compositor cursor (mod-held resize/move affordance, or active grab).
    void refreshInteractiveCursor();
    [[nodiscard]] bool compositorOwnsCursor() const { return m_compositorOwnsCursor; }

  private:
    static void onMotion(wl_listener* listener, void* data);
    static void onMotionAbsolute(wl_listener* listener, void* data);
    static void onButton(wl_listener* listener, void* data);
    static void onAxis(wl_listener* listener, void* data);
    static void onFrame(wl_listener* listener, void* data);
    static void onConstraintDestroy(wl_listener* listener, void* data);

    void handleMotion(void* data);
    void handleMotionAbsolute(void* data);
    void handleButton(void* data);
    void handleAxis(void* data);
    void handleFrame();
    void handleConstraintDestroy();

    void processMotion(uint32_t timeMsec, double oldX, double oldY);
    void processMove();
    void clipGrabbedViewToOutput();
    void updateDropTarget();
    void finishTileMove();
    void finishFloatMove();
    void processResize();
    void processResizeTile();
    [[nodiscard]] uint32_t tileResizeEdges(View* view) const;
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
    Workspace* m_dropWorkspace = nullptr;
    int m_dragSourceColumn = -1;
    int m_dropColumn = -1;
    int m_dropRow = -1;
    bool m_tileDragPending = false;
    double m_tileDragStartX = 0;
    double m_tileDragStartY = 0;
    Workspace* m_resizeWorkspace = nullptr;
    int m_resizeColumn = -1;
    int m_resizeRow = -1;
    double m_resizeStartX = 0;
    double m_resizeStartY = 0;
    double m_resizeStartScroll = 0;
    int m_resizeStartLeft = 0;
    int m_resizeStartRight = 0;
    int m_resizeStartWidthPx = 0;
    int m_resizeStartPrevWidthPx = 0;
    int m_resizeStartTop = 0;
    int m_resizeStartBottom = 0;
    double m_resizeStartUpperWeight = 0;
    double m_resizeStartLowerWeight = 0;
    int m_resizeUpperRow = -1;
    bool m_resizeSoloHorizontal = false;
    // Last layout output under the pointer; crossing heads updates seat focus like workspace switch.
    wlr_output* m_pointerOutput = nullptr;
    double m_wheelAccum[2]{};
    bool m_compositorOwnsCursor = false;
    std::string m_compositorCursorName;

    wl_listener m_motion{};
    wl_listener m_motionAbsolute{};
    wl_listener m_button{};
    wl_listener m_axis{};
    wl_listener m_frame{};
    wl_listener m_constraintDestroy{};
  };

} // namespace umbriel
