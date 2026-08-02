#pragma once

#include <cstdint>
#include <wayland-server-core.h>

struct wlr_cursor;
struct wlr_input_device;
struct wlr_pointer_constraint_v1;
struct wlr_surface;
struct wlr_xcursor_manager;

namespace umbriel {

  class Server;
  class View;

  enum class CursorMode {
    Passthrough,
    Move,
    Resize,
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

    void attachInputDevice(wlr_input_device* device);
    void beginInteractive(View* view, CursorMode mode, uint32_t edges);
    void resetMode();
    void handleNewConstraint(wlr_pointer_constraint_v1* constraint);
    void clearConstraint();

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

    void processMotion(uint32_t timeMsec);
    void processMove();
    void processResize();
    void setActiveConstraint(wlr_pointer_constraint_v1* constraint);
    void updateConstraintForSurface(wlr_surface* surface);
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

    wl_listener m_motion{};
    wl_listener m_motionAbsolute{};
    wl_listener m_button{};
    wl_listener m_axis{};
    wl_listener m_frame{};
    wl_listener m_constraintDestroy{};
  };

} // namespace umbriel
