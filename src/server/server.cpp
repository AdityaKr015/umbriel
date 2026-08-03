#include "server/server.h"

#include "config/config.h"
#include "config/config_watcher.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "layout/insert_hint.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace umbriel {

  namespace {

    constexpr Logger kLog("server");

    bool executableOnPath(const char* name) {
      if (name == nullptr || name[0] == '\0') {
        return false;
      }
      // Absolute / relative path: check directly.
      if (std::strchr(name, '/') != nullptr) {
        return access(name, X_OK) == 0;
      }
      const char* pathEnv = std::getenv("PATH");
      if (pathEnv == nullptr || pathEnv[0] == '\0') {
        return false;
      }
      std::string path = pathEnv;
      std::size_t start = 0;
      while (start <= path.size()) {
        const std::size_t end = path.find(':', start);
        const std::size_t count = (end == std::string::npos ? path.size() : end) - start;
        const std::string dir = path.substr(start, count);
        start = end == std::string::npos ? path.size() + 1 : end + 1;
        if (dir.empty()) {
          continue;
        }
        const std::filesystem::path candidate = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  Server::Server() {
    m_nested = std::getenv("WAYLAND_DISPLAY") != nullptr
        || std::getenv("WAYLAND_SOCKET") != nullptr
        || std::getenv("DISPLAY") != nullptr;

    m_display = wl_display_create();
    if (m_display == nullptr) {
      throw std::runtime_error("failed to create wl_display");
    }

    m_backend = wlr_backend_autocreate(wl_display_get_event_loop(m_display), &m_session);
    if (m_backend == nullptr) {
      throw std::runtime_error("failed to create wlr_backend");
    }

    m_renderer = fx_renderer_create(m_backend);
    if (m_renderer == nullptr) {
      throw std::runtime_error("failed to create fx_renderer");
    }
    wlr_renderer_init_wl_display(m_renderer, m_display);

    m_allocator = wlr_allocator_autocreate(m_backend, m_renderer);
    if (m_allocator == nullptr) {
      throw std::runtime_error("failed to create wlr_allocator");
    }

    m_compositor = wlr_compositor_create(m_display, 5, m_renderer);
    wlr_subcompositor_create(m_display);
    wlr_data_device_manager_create(m_display);
    wlr_primary_selection_v1_device_manager_create(m_display);
    wlr_viewporter_create(m_display);
    wlr_fractional_scale_manager_v1_create(m_display, 1);
    wlr_ext_data_control_manager_v1_create(m_display, 1);

    m_outputLayout = wlr_output_layout_create(m_display);
    wlr_xdg_output_manager_v1_create(m_display, m_outputLayout);
    m_scene = wlr_scene_create();
    const Config::Appearance::Blur& blur = config().appearance.blur;
    wlr_scene_set_blur_data(
        m_scene, blur.passes, blur.radius, static_cast<float>(blur.noise), static_cast<float>(blur.brightness),
        static_cast<float>(blur.contrast), static_cast<float>(blur.saturation)
    );
    m_sceneLayout = wlr_scene_attach_output_layout(m_scene, m_outputLayout);

    // Fixed global stacking: background < bottom < xdg < drag < top < fullscreen < overlay < lock.
    // Per-output layer trees are children of these roots so normal windows cannot raise above panels.
    // Fullscreen views are reparented into m_fullscreenTree so they cover top panels; overlay stays above.
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(&m_scene->tree);
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(&m_scene->tree);
    m_xdgTree = wlr_scene_tree_create(&m_scene->tree);
    m_dragTree = wlr_scene_tree_create(&m_scene->tree);
    m_dragIconTree = wlr_scene_tree_create(&m_scene->tree);
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&m_scene->tree);
    m_fullscreenTree = wlr_scene_tree_create(&m_scene->tree);
    m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(&m_scene->tree);
    m_lockTree = wlr_scene_tree_create(&m_scene->tree);
    const float blankColor[4] = {0.f, 0.f, 0.f, 1.f};
    m_lockBlank = wlr_scene_rect_create(m_lockTree, 0, 0, blankColor);
    wlr_scene_node_set_enabled(&m_lockBlank->node, false);
    wlr_scene_node_set_enabled(&m_lockTree->node, false);

    m_gammaManager = wlr_gamma_control_manager_v1_create(m_display);
    m_setGamma.notify = onSetGamma;
    wl_signal_add(&m_gammaManager->events.set_gamma, &m_setGamma);

    m_xdgShell = wlr_xdg_shell_create(m_display, 3);
    m_newXdgToplevel.notify = onNewXdgToplevel;
    wl_signal_add(&m_xdgShell->events.new_toplevel, &m_newXdgToplevel);
    m_newXdgPopup.notify = onNewXdgPopup;
    wl_signal_add(&m_xdgShell->events.new_popup, &m_newXdgPopup);

    m_xdgDecorationManager = wlr_xdg_decoration_manager_v1_create(m_display);
    m_newXdgDecoration.notify = onNewXdgDecoration;
    wl_signal_add(&m_xdgDecorationManager->events.new_toplevel_decoration, &m_newXdgDecoration);

    m_serverDecorationManager = wlr_server_decoration_manager_create(m_display);
    wlr_server_decoration_manager_set_default_mode(
        m_serverDecorationManager,
        config().general.preferNoCsd ? WLR_SERVER_DECORATION_MANAGER_MODE_SERVER
                                     : WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT
    );

    m_layerShell = wlr_layer_shell_v1_create(m_display, 4);
    m_newLayerSurface.notify = onNewLayerSurface;
    wl_signal_add(&m_layerShell->events.new_surface, &m_newLayerSurface);

    m_foreignToplevelManager = wlr_foreign_toplevel_manager_v1_create(m_display);

    m_workspaceManager = wlr_ext_workspace_manager_v1_create(m_display, 1);
    m_workspaceCommit.notify = onWorkspaceCommit;
    wl_signal_add(&m_workspaceManager->events.commit, &m_workspaceCommit);

    m_sessionLockManager = wlr_session_lock_manager_v1_create(m_display);
    m_newSessionLock.notify = onNewSessionLock;
    wl_signal_add(&m_sessionLockManager->events.new_lock, &m_newSessionLock);

    m_relativePointerManager = wlr_relative_pointer_manager_v1_create(m_display);
    m_pointerConstraints = wlr_pointer_constraints_v1_create(m_display);
    m_newPointerConstraint.notify = onNewPointerConstraint;
    wl_signal_add(&m_pointerConstraints->events.new_constraint, &m_newPointerConstraint);

    m_idleNotifier = wlr_idle_notifier_v1_create(m_display);
    m_idleInhibitManager = wlr_idle_inhibit_v1_create(m_display);
    m_newIdleInhibitor.notify = onNewIdleInhibitor;
    wl_signal_add(&m_idleInhibitManager->events.new_inhibitor, &m_newIdleInhibitor);

    wlr_screencopy_manager_v1_create(m_display);
    wlr_export_dmabuf_manager_v1_create(m_display);

    m_xdgActivation = wlr_xdg_activation_v1_create(m_display);
    m_requestActivate.notify = onRequestActivate;
    wl_signal_add(&m_xdgActivation->events.request_activate, &m_requestActivate);

    m_cursor = std::make_unique<Cursor>(*this);
    m_seat = std::make_unique<Seat>(*this);
    updateSeatCapabilities();

    m_newOutput.notify = onNewOutput;
    wl_signal_add(&m_backend->events.new_output, &m_newOutput);
    m_newInput.notify = onNewInput;
    wl_signal_add(&m_backend->events.new_input, &m_newInput);

    wlr_log(WLR_INFO, "mod key: %s (%s session)", m_nested ? "Alt" : "Super", m_nested ? "nested" : "native");
    kLog.info("mod key: {} ({} session)", m_nested ? "Alt" : "Super", m_nested ? "nested" : "native");
  }

  Server::~Server() {
    wl_list_remove(&m_newOutput.link);
    wl_list_remove(&m_newInput.link);
    wl_list_remove(&m_newXdgToplevel.link);
    wl_list_remove(&m_newXdgPopup.link);
    wl_list_remove(&m_newXdgDecoration.link);
    wl_list_remove(&m_newLayerSurface.link);
    wl_list_remove(&m_newSessionLock.link);
    wl_list_remove(&m_newPointerConstraint.link);
    wl_list_remove(&m_newIdleInhibitor.link);
    wl_list_remove(&m_requestActivate.link);
    wl_list_remove(&m_workspaceCommit.link);
    wl_list_remove(&m_setGamma.link);
    m_configWatcher.reset();

    m_insertHint.reset();
    m_sessionLock.reset();
    m_layerSurfaces.clear();
    m_views.clear();
    m_keyboards.clear();
    m_outputs.clear();
    m_seat.reset();
    m_cursor.reset();

    // Tear down xwayland-satellite before destroying Wayland clients.
    if (m_xwaylandExitSource != nullptr) {
      wl_event_source_remove(m_xwaylandExitSource);
    }
    if (m_xwaylandRespawnTimer != nullptr) {
      wl_event_source_remove(m_xwaylandRespawnTimer);
    }
    if (m_xwaylandPidfd >= 0) {
      close(m_xwaylandPidfd);
    }
    if (m_xwaylandPid > 0) {
      kill(m_xwaylandPid, SIGTERM);
    }
    wl_display_destroy_clients(m_display);
    wlr_scene_node_destroy(&m_scene->tree.node);
    wlr_allocator_destroy(m_allocator);
    wlr_renderer_destroy(m_renderer);
    wlr_backend_destroy(m_backend);
    wl_display_destroy(m_display);
  }

  bool Server::start(const char* startupCmd) {
    const char* socket = wl_display_add_socket_auto(m_display);
    if (socket == nullptr) {
      wlr_log(WLR_ERROR, "failed to add Wayland socket");
      return false;
    }
    m_socketName = socket;

    if (!wlr_backend_start(m_backend)) {
      wlr_log(WLR_ERROR, "failed to start backend");
      return false;
    }

    // Point new clients at us. Drop WAYLAND_SOCKET so children do not keep the
    // parent compositor connection (libwayland prefers it over WAYLAND_DISPLAY).
    setenv("WAYLAND_DISPLAY", m_socketName.c_str(), true);
    unsetenv("WAYLAND_SOCKET");
    kLog.info("running on WAYLAND_DISPLAY={}", m_socketName);
    wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", m_socketName.c_str());

    // Export cursor settings so X11 clients (via xwayland-satellite) and
    // toolkit clients pick up the compositor's configured cursor.
    const auto& cursorCfg = config().input.cursor;
    const std::string cursorSize = std::to_string(cursorCfg.size);
    setenv("XCURSOR_SIZE", cursorSize.c_str(), 1);
    if (!cursorCfg.theme.empty()) {
      setenv("XCURSOR_THEME", cursorCfg.theme.c_str(), 1);
    }

    // Start xwayland-satellite before autostart so X11 apps in autostart can
    // connect (there is still a small race against satellite's socket bind).
    if (config().general.xwayland) {
      startXwayland();
    }

    if (startupCmd != nullptr) {
      spawn(startupCmd);
    }
    for (const std::string& command : config().general.autostart) {
      spawn(command.c_str());
    }
    m_configWatcher =
        std::make_unique<ConfigWatcher>(wl_display_get_event_loop(m_display), [this] { handleConfigReload(); });
    m_configWatcher->watch(configWatchPaths());
    return true;
  }

  void Server::run() { wl_display_run(m_display); }

  void Server::stop() { wl_display_terminate(m_display); }

  uint32_t Server::modKey() const { return m_nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO; }

  InsertHint& Server::insertHint() {
    if (m_insertHint == nullptr) {
      m_insertHint = std::make_unique<InsertHint>(*this);
    }
    return *m_insertHint;
  }

  void Server::hideInsertHint() {
    if (m_insertHint != nullptr) {
      m_insertHint->hide();
    }
  }

  wlr_scene_tree* Server::shellLayerTree(uint32_t layer) const {
    if (layer >= kLayerCount) {
      return m_shellLayerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP];
    }
    return m_shellLayerTrees[layer];
  }

  void Server::spawn(const char* command) {
    if (m_socketName.empty()) {
      wlr_log(WLR_ERROR, "cannot spawn before the Wayland socket exists");
      return;
    }

    pid_t pid = fork();
    if (pid < 0) {
      wlr_log(WLR_ERROR, "fork failed");
      return;
    }
    if (pid == 0) {
      std::signal(SIGCHLD, SIG_DFL);
      setenv("WAYLAND_DISPLAY", m_socketName.c_str(), 1);
      unsetenv("WAYLAND_SOCKET");
      if (!m_xwaylandDisplay.empty()) {
        setenv("DISPLAY", m_xwaylandDisplay.c_str(), 1);
      } else {
        // Avoid X11/XWayland fallback into the parent session.
        unsetenv("DISPLAY");
      }
      execl("/bin/sh", "/bin/sh", "-c", command, nullptr);
      _exit(1);
    }

    wlr_log(WLR_INFO, "spawned '%s' on WAYLAND_DISPLAY=%s", command, m_socketName.c_str());
  }

  void Server::updateSeatCapabilities() { m_seat->updateCapabilities(!m_keyboards.empty()); }

  void Server::startXwayland() {
    if (!executableOnPath("xwayland-satellite")) {
      kLog.info("xwayland: xwayland-satellite not on PATH; skipping");
      return;
    }

    for (int n = 0; n < 32; ++n) {
      const std::string num = std::to_string(n);
      if (std::filesystem::exists("/tmp/.X11-unix/X" + num) || std::filesystem::exists("/tmp/.X" + num + "-lock")) {
        continue;
      }
      m_xwaylandDisplay = ":" + num;
      setenv("DISPLAY", m_xwaylandDisplay.c_str(), 1);
      kLog.info("xwayland: using DISPLAY={}", m_xwaylandDisplay);
      spawnXwaylandSatellite();
      return;
    }
    kLog.error("no free X display in :0..:31; disabling xwayland");
  }

  void Server::spawnXwaylandSatellite() {
    m_xwaylandSpawnTime = std::chrono::steady_clock::now();

    pid_t pid = fork();
    if (pid < 0) {
      kLog.error("fork failed for xwayland-satellite");
      return;
    }
    if (pid == 0) {
      std::signal(SIGCHLD, SIG_DFL);
      setenv("WAYLAND_DISPLAY", m_socketName.c_str(), 1);
      unsetenv("WAYLAND_SOCKET");
      // Satellite provides DISPLAY — it must not consume one.
      unsetenv("DISPLAY");
      execlp("xwayland-satellite", "xwayland-satellite", m_xwaylandDisplay.c_str(), nullptr);
      _exit(127);
    }

    m_xwaylandPid = pid;
    m_xwaylandPidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    if (m_xwaylandPidfd < 0) {
      kLog.error("pidfd_open failed for xwayland-satellite; crash respawn disabled");
    } else {
      m_xwaylandExitSource = wl_event_loop_add_fd(
          wl_display_get_event_loop(m_display), m_xwaylandPidfd, WL_EVENT_READABLE, onXwaylandPidfd, this
      );
    }
    kLog.info("xwayland-satellite spawned (pid {}) on DISPLAY={}", pid, m_xwaylandDisplay);
  }

  int Server::onXwaylandPidfd(int /*fd*/, uint32_t /*mask*/, void* data) {
    static_cast<Server*>(data)->handleXwaylandExit();
    return 0;
  }

  void Server::handleXwaylandExit() {
    int exitStatus = -1;
    if (m_xwaylandPidfd >= 0) {
      siginfo_t info{};
      if (waitid(P_PIDFD, static_cast<id_t>(m_xwaylandPidfd), &info, WEXITED | WNOHANG) == 0
          && info.si_code == CLD_EXITED) {
        exitStatus = info.si_status;
      }
    }

    // Clean up the current watch; no waitpid needed (SIGCHLD is SIG_IGN).
    if (m_xwaylandExitSource != nullptr) {
      wl_event_source_remove(m_xwaylandExitSource);
      m_xwaylandExitSource = nullptr;
    }
    if (m_xwaylandPidfd >= 0) {
      close(m_xwaylandPidfd);
      m_xwaylandPidfd = -1;
    }
    m_xwaylandPid = -1;

    if (exitStatus == 127) {
      kLog.error("xwayland-satellite not found or failed to exec; not respawning");
      m_xwaylandDisplay.clear();
      unsetenv("DISPLAY");
      return;
    }

    // Reset failure counter if the process ran for more than 60 seconds.
    auto elapsed = std::chrono::steady_clock::now() - m_xwaylandSpawnTime;
    if (elapsed > std::chrono::seconds(60)) {
      m_xwaylandFailures = 0;
    }
    ++m_xwaylandFailures;

    if (m_xwaylandFailures > 5) {
      kLog.error(
          "xwayland-satellite keeps exiting; giving up "
          "(is it installed and is Xwayland >= 23.1 present?)"
      );
      return;
    }

    kLog.warn("xwayland-satellite exited; respawning in 1 s");
    wl_event_loop* loop = wl_display_get_event_loop(m_display);
    if (m_xwaylandRespawnTimer == nullptr) {
      m_xwaylandRespawnTimer = wl_event_loop_add_timer(loop, onXwaylandRespawnTimer, this);
    }
    wl_event_source_timer_update(m_xwaylandRespawnTimer, 1000);
  }

  int Server::onXwaylandRespawnTimer(void* data) {
    static_cast<Server*>(data)->spawnXwaylandSatellite();
    return 0;
  }

} // namespace umbriel
