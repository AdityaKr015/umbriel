#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <wayland-server-core.h>

struct wlr_allocator;
struct wlr_backend;
struct wlr_box;
struct wlr_compositor;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_input_device;
struct wlr_layer_shell_v1;
struct wlr_output;
struct wlr_output_layout;
struct wlr_pointer_constraints_v1;
struct wlr_relative_pointer_manager_v1;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;
struct wlr_surface;
struct wlr_xdg_activation_v1;
struct wlr_xdg_shell;

namespace umbriel {

  class Cursor;
  class Keyboard;
  class LayerSurface;
  class Output;
  class Seat;
  class SessionLock;
  class View;

  class Server {
  public:
    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const char* startupCmd = nullptr);
    void run();
    void stop();

    [[nodiscard]] wl_display* display() const { return m_display; }
    [[nodiscard]] wlr_backend* backend() const { return m_backend; }
    [[nodiscard]] wlr_renderer* renderer() const { return m_renderer; }
    [[nodiscard]] wlr_allocator* allocator() const { return m_allocator; }
    [[nodiscard]] wlr_scene* scene() const { return m_scene; }
    [[nodiscard]] wlr_scene_tree* xdgTree() const { return m_xdgTree; }
    [[nodiscard]] wlr_scene_tree* lockTree() const { return m_lockTree; }
    [[nodiscard]] wlr_output_layout* outputLayout() const { return m_outputLayout; }
    [[nodiscard]] wlr_scene_output_layout* sceneLayout() const { return m_sceneLayout; }
    [[nodiscard]] Seat* seat() const { return m_seat.get(); }
    [[nodiscard]] Cursor* cursor() const { return m_cursor.get(); }
    [[nodiscard]] SessionLock* sessionLock() const { return m_sessionLock.get(); }
    [[nodiscard]] bool sessionLocked() const { return m_sessionLocked; }
    [[nodiscard]] wlr_foreign_toplevel_manager_v1* foreignToplevelManager() const { return m_foreignToplevelManager; }
    [[nodiscard]] wlr_pointer_constraints_v1* pointerConstraints() const { return m_pointerConstraints; }
    [[nodiscard]] wlr_relative_pointer_manager_v1* relativePointerManager() const { return m_relativePointerManager; }
    [[nodiscard]] bool nested() const { return m_nested; }
    [[nodiscard]] uint32_t modKey() const;

    void focusView(View* view);
    View* viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer = nullptr);
    bool handleKeybind(uint32_t keysym);
    void arrangeLayers(wlr_output* output);
    [[nodiscard]] wlr_output* preferredOutput() const;
    [[nodiscard]] Output* outputFromWlr(wlr_output* output) const;
    [[nodiscard]] wlr_box usableAreaAt(double lx, double ly) const;

    void removeOutput(Output* output);
    void removeKeyboard(Keyboard* keyboard);
    void removeView(View* view);
    void removeLayerSurface(LayerSurface* layerSurface, wlr_output* output);
    void removeSessionLock(SessionLock* lock);
    void unlockSession();
    void raiseLockTree();
    void updateLockBlank();
    void notifyIdleActivity();
    void refocus();
    [[nodiscard]] LayerSurface* exclusiveKeyboardLayer() const;

  private:
    friend class Output;
    friend class Keyboard;
    friend class Cursor;
    friend class View;
    friend class Seat;
    friend class LayerSurface;
    friend class SessionLock;
    friend class LockSurface;

    static void onNewOutput(wl_listener* listener, void* data);
    static void onNewInput(wl_listener* listener, void* data);
    static void onNewXdgToplevel(wl_listener* listener, void* data);
    static void onNewXdgPopup(wl_listener* listener, void* data);
    static void onNewLayerSurface(wl_listener* listener, void* data);
    static void onNewSessionLock(wl_listener* listener, void* data);
    static void onNewPointerConstraint(wl_listener* listener, void* data);
    static void onNewIdleInhibitor(wl_listener* listener, void* data);
    static void onIdleInhibitorDestroy(wl_listener* listener, void* data);
    static void onRequestActivate(wl_listener* listener, void* data);

    void addOutput(wlr_output* output);
    void addKeyboard(wlr_input_device* device);
    void addPointer(wlr_input_device* device);
    void updateSeatCapabilities();
    void spawn(const char* command);
    void beginSessionLock(wlr_session_lock_v1* lock);
    void clearNormalFocus();
    void setLockBlankEnabled(bool enabled);
    void updateIdleInhibit();

    struct IdleInhibitorWatch {
      Server* server = nullptr;
      wl_listener destroy{};
    };

    wl_display* m_display = nullptr;
    wlr_backend* m_backend = nullptr;
    wlr_renderer* m_renderer = nullptr;
    wlr_allocator* m_allocator = nullptr;
    wlr_compositor* m_compositor = nullptr;
    wlr_output_layout* m_outputLayout = nullptr;
    wlr_scene* m_scene = nullptr;
    wlr_scene_output_layout* m_sceneLayout = nullptr;
    wlr_xdg_shell* m_xdgShell = nullptr;
    wlr_layer_shell_v1* m_layerShell = nullptr;
    wlr_foreign_toplevel_manager_v1* m_foreignToplevelManager = nullptr;
    wlr_session_lock_manager_v1* m_sessionLockManager = nullptr;
    wlr_pointer_constraints_v1* m_pointerConstraints = nullptr;
    wlr_relative_pointer_manager_v1* m_relativePointerManager = nullptr;
    wlr_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
    wlr_idle_notifier_v1* m_idleNotifier = nullptr;
    wlr_xdg_activation_v1* m_xdgActivation = nullptr;
    wlr_scene_tree* m_xdgTree = nullptr;
    wlr_scene_tree* m_lockTree = nullptr;
    wlr_scene_rect* m_lockBlank = nullptr;
    bool m_sessionLocked = false;

    std::unique_ptr<Seat> m_seat;
    std::unique_ptr<Cursor> m_cursor;
    std::unique_ptr<SessionLock> m_sessionLock;

    bool m_nested = false;
    std::string m_socketName;

    wl_listener m_newOutput{};
    wl_listener m_newInput{};
    wl_listener m_newXdgToplevel{};
    wl_listener m_newXdgPopup{};
    wl_listener m_newLayerSurface{};
    wl_listener m_newSessionLock{};
    wl_listener m_newPointerConstraint{};
    wl_listener m_newIdleInhibitor{};
    wl_listener m_requestActivate{};

    std::vector<std::unique_ptr<Output>> m_outputs;
    std::vector<std::unique_ptr<Keyboard>> m_keyboards;
    std::vector<std::unique_ptr<View>> m_views;
    std::vector<std::unique_ptr<LayerSurface>> m_layerSurfaces;
  };

} // namespace umbriel
