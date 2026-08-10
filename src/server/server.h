#pragma once
#include "core/animation.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

struct wlr_allocator;
struct wlr_backend;
struct wlr_box;
struct wlr_compositor;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_input_device;
struct wlr_layer_shell_v1;
struct wlr_output;
struct wlr_output_layout;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_gestures_v1;
struct wlr_relative_pointer_manager_v1;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_session;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;
struct wlr_surface;
struct wlr_xdg_activation_v1;
struct wlr_xdg_decoration_manager_v1;
struct wlr_xdg_shell;
struct wlr_server_decoration_manager;
struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_ext_workspace_manager_v1;
struct wlr_gamma_control_manager_v1;
struct wlr_output_manager_v1;
struct wlr_output_configuration_v1;
struct wlr_scene_tree;
struct wlr_virtual_pointer_manager_v1;
struct wlr_virtual_pointer_v1;

namespace umbriel {

  enum class WheelDirection;
  struct Keybind;

  enum class FocusReason : uint8_t {
    Directional,  // window-focus-* keybinds, focus-adjacent after close
    PointerPress, // plain click-to-focus
    PointerHover, // follows_mouse enter
    Grab,         // Mod+drag / Mod+resize start
    DragDrop,     // tile/float drag finished
    Startup,      // map, setFloating, xdg-activation, foreign activate, refocus fallback
  };

  class Cursor;
  class ConfigWatcher;
  class Gestures;
  class InsertHint;
  class Keyboard;
  class LayerSurface;
  class Output;
  class Overview;
  class Seat;
  class SessionLock;
  class View;
  class Workspace;
  class WorkspaceGroup;
  class ConfigBanner;
  class Cheatsheet;
  class Ipc;

  class Server {
  public:
    static constexpr uint32_t kLayerCount = 4;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const char* startupCmd = nullptr);
    void run();
    void stop();

    [[nodiscard]] wl_display* display() const { return m_display; }
    [[nodiscard]] wlr_backend* backend() const { return m_backend; }
    [[nodiscard]] wlr_session* session() const { return m_session; }
    [[nodiscard]] wlr_renderer* renderer() const { return m_renderer; }
    [[nodiscard]] wlr_allocator* allocator() const { return m_allocator; }
    [[nodiscard]] wlr_scene* scene() const { return m_scene; }
    [[nodiscard]] wlr_scene_tree* xdgTree() const { return m_xdgTree; }
    // Between windows and the drag/insert-hint tree: overview cards render here
    // while the real window trees are disabled.
    [[nodiscard]] wlr_scene_tree* overviewTree() const { return m_overviewTree; }
    [[nodiscard]] Overview* overview() const { return m_overview.get(); }
    [[nodiscard]] Cheatsheet* cheatsheet() const { return m_cheatsheet.get(); }
    // Above xdg windows, below layer-shell top/overlay (drag/drop insert hint).
    [[nodiscard]] wlr_scene_tree* dragTree() const { return m_dragTree; }
    // Parent for wl_data_device drag icons; moved to the cursor while a drag is active.
    [[nodiscard]] wlr_scene_tree* dragIconTree() const { return m_dragIconTree; }
    // Above top panels, below overlay/lock (fullscreen xdg views).
    [[nodiscard]] wlr_scene_tree* fullscreenTree() const { return m_fullscreenTree; }
    [[nodiscard]] wlr_scene_tree* lockTree() const { return m_lockTree; }
    [[nodiscard]] wlr_scene_tree* shellLayerTree(uint32_t layer) const;
    [[nodiscard]] wlr_output_layout* outputLayout() const { return m_outputLayout; }
    [[nodiscard]] wlr_scene_output_layout* sceneLayout() const { return m_sceneLayout; }
    [[nodiscard]] Seat* seat() const { return m_seat.get(); }
    [[nodiscard]] Cursor* cursor() const { return m_cursor.get(); }
    [[nodiscard]] Animator& animator() { return m_animator; }
    [[nodiscard]] const Animator& animator() const { return m_animator; }
    [[nodiscard]] InsertHint& insertHint();
    void hideInsertHint();
    [[nodiscard]] SessionLock* sessionLock() const { return m_sessionLock.get(); }
    [[nodiscard]] bool sessionLocked() const { return m_sessionLocked; }
    [[nodiscard]] const std::string& activeSubmap() const {
      static const std::string empty;
      return m_activeSubmaps.empty() ? empty : m_activeSubmaps.back();
    }
    void pushSubmap(const std::string& name);
    void popSubmap();
    [[nodiscard]] wlr_foreign_toplevel_manager_v1* foreignToplevelManager() const { return m_foreignToplevelManager; }
    [[nodiscard]] wlr_ext_foreign_toplevel_list_v1* extForeignToplevelList() const { return m_extForeignToplevelList; }
    [[nodiscard]] wlr_ext_workspace_manager_v1* workspaceManager() const { return m_workspaceManager; }
    [[nodiscard]] wlr_gamma_control_manager_v1* gammaManager() const { return m_gammaManager; }
    [[nodiscard]] wlr_pointer_constraints_v1* pointerConstraints() const { return m_pointerConstraints; }
    [[nodiscard]] wlr_relative_pointer_manager_v1* relativePointerManager() const { return m_relativePointerManager; }
    [[nodiscard]] wlr_pointer_gestures_v1* pointerGestures() const { return m_pointerGestures; }
    [[nodiscard]] wlr_virtual_pointer_manager_v1* virtualPointerManager() const { return m_virtualPointerManager; }
    [[nodiscard]] Gestures* gestures() const { return m_gestures.get(); }
    [[nodiscard]] bool nested() const { return m_nested; }
    [[nodiscard]] uint32_t modKey() const;

    void focusView(View* view, FocusReason reason = FocusReason::Startup);
    View* viewAt(double lx, double ly, wlr_surface** surface, double* sx, double* sy, LayerSurface** layer = nullptr);
    const Keybind* handleKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers);
    bool handleWheelBind(WheelDirection direction, uint32_t modifiers);
    bool handleMouseBind(uint32_t button, uint32_t modifiers);
    bool handleVtSwitch(uint32_t keysym, uint32_t modifiers);
    void arrangeLayers(wlr_output* output);
    [[nodiscard]] wlr_output* preferredOutput() const;
    [[nodiscard]] Output* outputFromWlr(wlr_output* output) const;
    [[nodiscard]] Output* outputFromName(const std::string& name) const;
    [[nodiscard]] wlr_box usableAreaAt(double lx, double ly) const;

    void removeOutput(Output* output);
    void removeKeyboard(Keyboard* keyboard);
    void removeView(View* view);
    void removeLayerSurface(LayerSurface* layerSurface, wlr_output* output);
    void removeSessionLock(SessionLock* lock);
    void unlockSession();
    void raiseLockTree();
    void updateLockBlank();
    void updateBackdrop();
    void notifyIdleActivity();
    // Prefer focusing a view on `preferred` when set (workspace switch on one output).
    void refocus(Output* preferred = nullptr);
    void clearKeyboardFocus();
    // Drop xdg activated / focus border / foreign activated on mapped views (except `except`).
    void deactivateViews(View* except = nullptr);
    [[nodiscard]] LayerSurface* exclusiveKeyboardLayer() const;
    void
    animateCloseSnapshot(wlr_scene_tree* tree, std::vector<std::pair<wlr_scene_rect*, std::array<float, 4>>> rects);

  private:
    friend class Output;
    friend class Keyboard;
    friend class Cursor;
    friend class View;
    friend class Seat;
    friend class LayerSurface;
    friend class SessionLock;
    friend class LockSurface;
    friend class Ipc;
    friend class Overview;

    static void onNewOutput(wl_listener* listener, void* data);
    static void onNewInput(wl_listener* listener, void* data);
    static void onNewXdgToplevel(wl_listener* listener, void* data);
    static void onNewXdgPopup(wl_listener* listener, void* data);
    static void onNewXdgDecoration(wl_listener* listener, void* data);
    static void onNewLayerSurface(wl_listener* listener, void* data);
    static void onNewSessionLock(wl_listener* listener, void* data);
    static void onNewPointerConstraint(wl_listener* listener, void* data);
    static void onNewVirtualPointer(wl_listener* listener, void* data);
    static void onVirtualPointerMotion(wl_listener* listener, void* data);
    static void onVirtualPointerMotionAbsolute(wl_listener* listener, void* data);
    static void onVirtualPointerButton(wl_listener* listener, void* data);
    static void onVirtualPointerAxis(wl_listener* listener, void* data);
    static void onVirtualPointerFrame(wl_listener* listener, void* data);
    static void onVirtualPointerDestroy(wl_listener* listener, void* data);
    static void onNewIdleInhibitor(wl_listener* listener, void* data);
    static void onIdleInhibitorDestroy(wl_listener* listener, void* data);
    static void onRequestActivate(wl_listener* listener, void* data);
    static void onWorkspaceCommit(wl_listener* listener, void* data);
    static void onSetGamma(wl_listener* listener, void* data);
    static void onPointerDestroy(wl_listener* listener, void* data);
    static void onTouchDestroy(wl_listener* listener, void* data);
    static void onOutputManagerApply(wl_listener* listener, void* data);
    static void onOutputManagerTest(wl_listener* listener, void* data);
    static void onOutputLayoutChange(wl_listener* listener, void* data);
    static void onToplevelCaptureRequest(wl_listener* listener, void* data);

    void addOutput(wlr_output* output);
    void addKeyboard(wlr_input_device* device);
    void addPointer(wlr_input_device* device);
    void addTouch(wlr_input_device* device);
    void updateSeatCapabilities();
    void spawn(const char* command);
    void beginSessionLock(wlr_session_lock_v1* lock);
    void applyConfig();
    void handleConfigReload();
    bool executeKeybindAction(const Keybind& bind, std::string* error = nullptr);
    void showConfigDiagnostics();
    void relayoutBanner();
    void relayoutCheatsheet();
    void clearNormalFocus();
    void setLockBlankEnabled(bool enabled);
    void updateIdleInhibit();
    void handleWorkspaceCommit(void* data);
    [[nodiscard]] Workspace* workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const;
    void updateOutputManagerConfig();
    void applyOutputManagerConfig(wlr_output_configuration_v1* config, bool testOnly);
    [[nodiscard]] WorkspaceGroup* workspaceGroupFromHandle(wlr_ext_workspace_group_handle_v1* handle) const;

    // xwayland-satellite lifecycle
    void startXwayland();
    void spawnXwaylandSatellite();
    void handleXwaylandExit();
    static int onXwaylandPidfd(int fd, uint32_t mask, void* data);
    static int onXwaylandRespawnTimer(void* data);

    struct IdleInhibitorWatch {
      Server* server = nullptr;
      wl_listener destroy{};
    };
    struct PointerDevice {
      Server* server = nullptr;
      wlr_input_device* device = nullptr;
      wl_listener destroy{};
    };
    struct TouchDevice {
      Server* server = nullptr;
      wlr_input_device* device = nullptr;
      wl_listener destroy{};
    };
    struct VirtualPointerDevice {
      Server* server = nullptr;
      wlr_virtual_pointer_v1* vpointer = nullptr;
      wl_listener motion{};
      wl_listener motionAbsolute{};
      wl_listener button{};
      wl_listener axis{};
      wl_listener frame{};
      wl_listener destroy{};
    };

    wl_display* m_display = nullptr;
    wlr_backend* m_backend = nullptr;
    wlr_session* m_session = nullptr;
    wlr_renderer* m_renderer = nullptr;
    wlr_allocator* m_allocator = nullptr;
    wlr_compositor* m_compositor = nullptr;
    wlr_output_layout* m_outputLayout = nullptr;
    wlr_scene* m_scene = nullptr;
    wlr_scene_output_layout* m_sceneLayout = nullptr;
    wlr_xdg_shell* m_xdgShell = nullptr;
    wlr_xdg_decoration_manager_v1* m_xdgDecorationManager = nullptr;
    wlr_server_decoration_manager* m_serverDecorationManager = nullptr;
    wlr_layer_shell_v1* m_layerShell = nullptr;
    wlr_foreign_toplevel_manager_v1* m_foreignToplevelManager = nullptr;
    wlr_ext_foreign_toplevel_list_v1* m_extForeignToplevelList = nullptr;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1* m_toplevelCaptureSourceManager = nullptr;
    wlr_ext_workspace_manager_v1* m_workspaceManager = nullptr;
    wlr_session_lock_manager_v1* m_sessionLockManager = nullptr;
    wlr_pointer_constraints_v1* m_pointerConstraints = nullptr;
    wlr_relative_pointer_manager_v1* m_relativePointerManager = nullptr;
    wlr_pointer_gestures_v1* m_pointerGestures = nullptr;
    wlr_virtual_pointer_manager_v1* m_virtualPointerManager = nullptr;
    wlr_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
    wlr_idle_notifier_v1* m_idleNotifier = nullptr;
    wlr_xdg_activation_v1* m_xdgActivation = nullptr;
    wlr_gamma_control_manager_v1* m_gammaManager = nullptr;
    wlr_output_manager_v1* m_outputManager = nullptr;
    wlr_scene_tree* m_shellLayerTrees[kLayerCount]{};
    wlr_scene_tree* m_xdgTree = nullptr;
    wlr_scene_tree* m_overviewTree = nullptr;
    wlr_scene_tree* m_dragTree = nullptr;
    wlr_scene_tree* m_dragIconTree = nullptr;
    wlr_scene_tree* m_fullscreenTree = nullptr;
    wlr_scene_tree* m_lockTree = nullptr;
    wlr_scene_rect* m_lockBlank = nullptr;
    wlr_scene_rect* m_backdrop = nullptr;
    bool m_sessionLocked = false;
    std::vector<std::string> m_activeSubmaps;
    Animator m_animator;

    struct CloseSnapshot {
      wlr_scene_tree* tree = nullptr;
      AnimId anim = 0;
      std::vector<std::pair<wlr_scene_rect*, std::array<float, 4>>> rects;
    };
    std::vector<CloseSnapshot> m_closeSnapshots;

    std::unique_ptr<Seat> m_seat;
    std::unique_ptr<Cursor> m_cursor;
    std::unique_ptr<Gestures> m_gestures;
    std::unique_ptr<SessionLock> m_sessionLock;
    std::unique_ptr<Overview> m_overview;
    std::unique_ptr<InsertHint> m_insertHint;
    std::unique_ptr<ConfigWatcher> m_configWatcher;
    std::unique_ptr<Ipc> m_ipc;
    wlr_scene_tree* m_bannerTree = nullptr;
    std::unique_ptr<ConfigBanner> m_configBanner;
    wlr_scene_tree* m_cheatsheetTree = nullptr;
    std::unique_ptr<Cheatsheet> m_cheatsheet;

    bool m_nested = false;
    std::string m_socketName;

    // xwayland-satellite state
    pid_t m_xwaylandPid = -1;
    int m_xwaylandPidfd = -1;
    std::string m_xwaylandDisplay;
    wl_event_source* m_xwaylandExitSource = nullptr;
    wl_event_source* m_xwaylandRespawnTimer = nullptr;
    int m_xwaylandFailures = 0;
    std::chrono::steady_clock::time_point m_xwaylandSpawnTime;

    wl_listener m_newOutput{};
    wl_listener m_newInput{};
    wl_listener m_newXdgToplevel{};
    wl_listener m_newXdgPopup{};
    wl_listener m_newXdgDecoration{};
    wl_listener m_newLayerSurface{};
    wl_listener m_newSessionLock{};
    wl_listener m_newPointerConstraint{};
    wl_listener m_newVirtualPointer{};
    wl_listener m_newIdleInhibitor{};
    wl_listener m_requestActivate{};
    wl_listener m_workspaceCommit{};
    wl_listener m_setGamma{};
    wl_listener m_outputManagerApply{};
    wl_listener m_outputManagerTest{};
    wl_listener m_outputLayoutChange{};
    wl_listener m_toplevelCaptureRequest{};

    std::vector<std::unique_ptr<Output>> m_outputs;
    std::vector<std::unique_ptr<Keyboard>> m_keyboards;
    std::vector<std::unique_ptr<PointerDevice>> m_pointers;
    std::vector<std::unique_ptr<TouchDevice>> m_touchDevices;
    std::vector<std::unique_ptr<VirtualPointerDevice>> m_virtualPointers;
    std::vector<std::unique_ptr<View>> m_views;
    std::vector<std::unique_ptr<LayerSurface>> m_layerSurfaces;
  };

} // namespace umbriel
