#include "config/change.h"
#include "config/config.h"
#include "config/config_watcher.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/gestures.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/hint_rect.h"
#include "server/server.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("server");

    View* viewForSurface(Server& server, wlr_surface* surface) {
      if (surface == nullptr) {
        return nullptr;
      }
      wlr_surface* root = wlr_surface_get_root_surface(surface);
      wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
      if (toplevel == nullptr) {
        return nullptr;
      }
      for (const auto& entry : server.registry().all()) {
        if (entry->toplevel() == toplevel) {
          return entry.get();
        }
      }
      return nullptr;
    }

    pid_t surfaceClientPid(wlr_surface* surface) {
      if (surface == nullptr || surface->resource == nullptr) {
        return -1;
      }
      pid_t pid = -1;
      uid_t uid = 0;
      gid_t gid = 0;
      wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, &uid, &gid);
      return pid;
    }

    const char* deviceName(const wlr_input_device* device) {
      return device->name != nullptr ? device->name : "unknown";
    }

    wlr_xdg_toplevel_decoration_v1_mode resolvedDecorationMode(wlr_xdg_toplevel_decoration_v1* decoration) {
      // Honor an explicit client request; otherwise prefer SSD when configured.
      wlr_xdg_toplevel_decoration_v1_mode mode = decoration->requested_mode;
      if (mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE) {
        mode = config().appearance.preferNoCsd ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                               : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
      }
      return mode;
    }

    struct XdgDecorationWatch {
      wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
      wlr_xdg_toplevel_decoration_v1_mode pendingMode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
      wl_listener requestMode{};
      wl_listener surfaceCommit{};
      wl_listener destroy{};
    };

    void clearDecorationCommit(XdgDecorationWatch* watch) {
      if (watch->surfaceCommit.notify == nullptr) {
        return;
      }
      wl_list_remove(&watch->surfaceCommit.link);
      watch->surfaceCommit.notify = nullptr;
    }

    void onDecorationSurfaceCommit(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, surfaceCommit);
      if (watch->decoration == nullptr
          || watch->decoration->toplevel == nullptr
          || !watch->decoration->toplevel->base->initial_commit) {
        return;
      }
      wlr_xdg_toplevel_decoration_v1_set_mode(watch->decoration, watch->pendingMode);
      clearDecorationCommit(watch);
    }

    void applyXdgDecorationMode(XdgDecorationWatch* watch) {
      if (watch == nullptr || watch->decoration == nullptr || watch->decoration->toplevel == nullptr) {
        return;
      }
      wlr_xdg_surface* surface = watch->decoration->toplevel->base;
      if (surface == nullptr) {
        return;
      }
      watch->pendingMode = resolvedDecorationMode(watch->decoration);
      // set_mode schedules a configure; that asserts unless the xdg_surface is ready.
      if (surface->initialized) {
        clearDecorationCommit(watch);
        wlr_xdg_toplevel_decoration_v1_set_mode(watch->decoration, watch->pendingMode);
        return;
      }
      if (watch->surfaceCommit.notify != nullptr) {
        return;
      }
      watch->surfaceCommit.notify = onDecorationSurfaceCommit;
      wl_signal_add(&surface->surface->events.commit, &watch->surfaceCommit);
    }

    void applyKdeDecorationDefault(wlr_server_decoration_manager* manager) {
      if (manager == nullptr) {
        return;
      }
      const uint32_t mode = config().appearance.preferNoCsd ? WLR_SERVER_DECORATION_MANAGER_MODE_SERVER
                                                            : WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
      wlr_server_decoration_manager_set_default_mode(manager, mode);
    }

    void onDecorationRequestMode(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, requestMode);
      applyXdgDecorationMode(watch);
    }

    void onDecorationDestroy(wl_listener* listener, void* /*data*/) {
      XdgDecorationWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      if (watch->decoration != nullptr) {
        watch->decoration->data = nullptr;
      }
      wl_list_remove(&watch->requestMode.link);
      clearDecorationCommit(watch);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

    void applyNaturalScroll(
        libinput_device* libinputDevice, const wlr_input_device* device, const std::optional<bool>& configured,
        std::string_view setting
    ) {
      if (libinput_device_config_scroll_has_natural_scroll(libinputDevice) == 0) {
        if (configured) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), setting);
        }
        return;
      }
      const bool enabled =
          configured.value_or(libinput_device_config_scroll_get_default_natural_scroll_enabled(libinputDevice) != 0);
      if (libinput_device_config_scroll_set_natural_scroll_enabled(libinputDevice, enabled)
          != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        kLog.warn("input: failed to apply {} to '{}'", setting, deviceName(device));
      }
    }

    void applyPointerConfig(wlr_input_device* device) {
      if (!wlr_input_device_is_libinput(device)) {
        return;
      }
      libinput_device* libinputDevice = wlr_libinput_get_device_handle(device);
      if (libinputDevice == nullptr) {
        return;
      }

      const Config::Input& input = config().input;
      const Config::Input::Device* override = input.findDevice(device->name != nullptr ? device->name : "");
      const bool isTouchpad = libinput_device_config_tap_get_finger_count(libinputDevice) > 0;
      if (isTouchpad) {
        const std::optional<bool>& tap = override != nullptr && override->tap ? override->tap : input.touchpad.tap;
        const auto tapState =
            tap.value_or(libinput_device_config_tap_get_default_enabled(libinputDevice) == LIBINPUT_CONFIG_TAP_ENABLED)
            ? LIBINPUT_CONFIG_TAP_ENABLED
            : LIBINPUT_CONFIG_TAP_DISABLED;
        if (libinput_device_config_tap_set_enabled(libinputDevice, tapState) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn(
              "input: failed to apply {} to '{}'",
              override != nullptr && override->tap ? "input.device.tap" : "input.touchpad.tap", deviceName(device)
          );
        }

        const std::optional<bool>& naturalScroll =
            override != nullptr && override->naturalScroll ? override->naturalScroll : input.touchpad.naturalScroll;
        applyNaturalScroll(
            libinputDevice, device, naturalScroll,
            override != nullptr && override->naturalScroll ? "input.device.natural_scroll"
                                                           : "input.touchpad.natural_scroll"
        );
        return;
      }

      const std::optional<bool>& naturalScroll =
          override != nullptr && override->naturalScroll ? override->naturalScroll : input.mouse.naturalScroll;
      applyNaturalScroll(
          libinputDevice, device, naturalScroll,
          override != nullptr && override->naturalScroll ? "input.device.natural_scroll" : "input.mouse.natural_scroll"
      );
    }
  } // namespace
  void Server::applyConfig(const ConfigEffects& effects) {
    if (!effects.any()) {
      return;
    }

    if (effects.sceneBlur) {
      const Config::Appearance::Blur& blur = config().appearance.blur;
      wlr_scene_set_blur_data(
          m_scene, blur.passes, blur.radius, static_cast<float>(blur.noise), static_cast<float>(blur.brightness),
          static_cast<float>(blur.contrast), static_cast<float>(blur.saturation)
      );
    }
    if (effects.input) {
      for (const auto& keyboard : m_keyboards) {
        keyboard->applyConfig();
      }
      for (const auto& pointer : m_pointers) {
        applyPointerConfig(pointer->device);
      }
      m_cursor->applyConfig();
    }
    if (effects.internalUi) {
      markDirty(Dirty::Cheatsheet);
    }
    if (effects.outputState) {
      for (const auto& output : m_outputs) {
        output->applyOutputState();
      }
      updateOutputManagerConfig();
    }
    if (effects.workspaceInventory) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->reconcileInventory();
        }
      }
    }
    if (effects.workspaceLayout) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->refreshLayouts();
        }
      }
      m_cursor->cancelStaleTiledResize();
    }
    if (effects.viewChrome) {
      for (const auto& view : m_registry.all()) {
        if (view->mapped()) {
          view->refreshConfigChrome();
        }
      }
      applyKdeDecorationDefault(m_serverDecorationManager);
      if (m_xdgDecorationManager != nullptr) {
        wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
        wl_list_for_each(decoration, &m_xdgDecorationManager->decorations, link) {
          if (auto* watch = static_cast<XdgDecorationWatch*>(decoration->data)) {
            applyXdgDecorationMode(watch);
          }
        }
      }
      // The view refresh cleared every focus ring; put the active one back.
      refocus();
      markDirty(Dirty::Backdrop);
      if (m_sessionLocked) {
        updateLockBlank();
      }
    }
    if (effects.layerEffects) {
      for (const auto& layer : m_layerSurfaces) {
        if (layer->mapped()) {
          layer->applyConfig();
        }
      }
    }
  }

  void Server::handleConfigReload() {
    cancelModifierTap();
    const ConfigReloadResult result = reloadConfig();
    if (result.success) {
      if (result.effects.invalidatesOverview()) {
        m_overview->forceClose();
      }
      applyConfig(result.effects);
      const std::string changed = result.change.summary();
      const std::string effects = result.effects.summary();
      kLog.info(
          "config reloaded (sections: {}; effects: {})", changed.empty() ? "none" : changed,
          effects.empty() ? "none" : effects
      );
    }
    showConfigDiagnostics();
    markDirty(Dirty::Cheatsheet);
    if (m_configWatcher != nullptr) {
      m_configWatcher->watch(configWatchPaths());
    }
  }

  // Slow tick that keeps hidden-workspace toplevels driving their game/network
  // loops (see kBackgroundFrameIntervalMs). wlr_scene_output_send_frame_done
  // walks only enabled scene nodes, so a view whose workspace has been
  // deactivated stops receiving wl_surface.frame callbacks entirely; any
  // client that gates advance-work on the callback stalls until it is shown
  // again.
  int Server::onBackgroundFrameTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (const auto& view : self->m_registry.all()) {
      if (!view->mapped() || view->onActiveWorkspace()) {
        continue;
      }
      wlr_xdg_surface_for_each_surface(
          view->toplevel()->base,
          [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* userData) {
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(userData));
          },
          &now
      );
    }

    if (self->m_backgroundFrameTimer != nullptr) {
      wl_event_source_timer_update(self->m_backgroundFrameTimer, kBackgroundFrameIntervalMs);
    }
    return 0;
  }

  // Fires when the underlying GL context is invalidated (GPU reset, VRAM lost after suspend,
  // driver-detected hang). Without this, the renderer keeps issuing GL calls into a dead
  // context — Mesa's context_lost_nop_handler no-ops each one and spams "[GLES2]
  // GL_CONTEXT_LOST in context lost" ~40k lines/sec, and the desktop never comes back.
  // Rebuild the renderer and rebind everything.
  void Server::onRendererLost(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_rendererLost);
    self->recreateRenderer();
  }

  void Server::recreateRenderer() {
    kLog.warn("GPU context lost — recreating renderer");

    wlr_renderer* oldRenderer = m_renderer;
    wlr_allocator* oldAllocator = m_allocator;

    wlr_renderer* newRenderer = fx_renderer_create(m_backend);
    if (newRenderer == nullptr) {
      kLog.error("could not recreate fx_renderer after GPU reset — terminating");
      wl_display_terminate(m_display);
      return;
    }
    wlr_allocator* newAllocator = wlr_allocator_autocreate(m_backend, newRenderer);
    if (newAllocator == nullptr) {
      kLog.error("could not recreate allocator after GPU reset — terminating");
      wlr_renderer_destroy(newRenderer);
      wl_display_terminate(m_display);
      return;
    }

    // Rewire the lost signal onto the new renderer BEFORE swapping the pointers so that
    // a second reset during recreation is delivered.
    wl_list_remove(&m_rendererLost.link);
    wl_signal_add(&newRenderer->events.lost, &m_rendererLost);

    m_renderer = newRenderer;
    m_allocator = newAllocator;

    // Point the compositor at the new renderer so clients' shm/dma-buf textures get
    // re-imported on next attach.
    wlr_compositor_set_renderer(m_compositor, newRenderer);

    // Re-init every output's render pipeline with the new renderer/allocator, and force
    // a fresh frame so damage tracking rebuilds from scratch.
    for (const auto& output : m_outputs) {
      wlr_output* wlrOutput = output->wlr();
      wlr_output_init_render(wlrOutput, newAllocator, newRenderer);
      wlr_output_schedule_frame(wlrOutput);
    }

    wlr_allocator_destroy(oldAllocator);
    wlr_renderer_destroy(oldRenderer);

    kLog.info("renderer recreated");
  }

  void Server::onNewOutput(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newOutput);
    self->addOutput(static_cast<wlr_output*>(data));
  }

  void Server::onNewInput(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newInput);
    auto* device = static_cast<wlr_input_device*>(data);
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
      self->addKeyboard(device);
      break;
    case WLR_INPUT_DEVICE_POINTER:
      self->addPointer(device);
      break;
    case WLR_INPUT_DEVICE_TOUCH:
      self->addTouch(device);
      break;
    default:
      break;
    }
    self->updateSeatCapabilities();
  }

  void Server::onNewXdgToplevel(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newXdgToplevel);
    self->m_registry.add(std::make_unique<View>(*self, static_cast<wlr_xdg_toplevel*>(data)));
  }

  void Server::onNewXdgPopup(wl_listener* /*listener*/, void* data) {
    auto* popup = static_cast<wlr_xdg_popup*>(data);
    // Layer-shell popups (and any popup without a parent yet) are handled
    // elsewhere; parent can be null when xdg_shell emits new_popup.
    if (popup->parent == nullptr || wlr_xdg_surface_try_from_wlr_surface(popup->parent) == nullptr) {
      return;
    }
    new Popup(popup);
  }

  void Server::onNewXdgDecoration(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newXdgDecoration);
    auto* decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);
    auto* watch = new XdgDecorationWatch{.decoration = decoration};
    decoration->data = watch;
    watch->requestMode.notify = onDecorationRequestMode;
    wl_signal_add(&decoration->events.request_mode, &watch->requestMode);
    watch->destroy.notify = onDecorationDestroy;
    wl_signal_add(&decoration->events.destroy, &watch->destroy);
    applyXdgDecorationMode(watch);
    (void)self;
  }

  void Server::onNewLayerSurface(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newLayerSurface);
    auto surface = std::make_unique<LayerSurface>(*self, static_cast<wlr_layer_surface_v1*>(data));
    if (surface->layerSurface() == nullptr) {
      return;
    }
    self->m_layerSurfaces.push_back(std::move(surface));
  }

  void Server::onNewSessionLock(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newSessionLock);
    self->beginSessionLock(static_cast<wlr_session_lock_v1*>(data));
  }

  void Server::onNewPointerConstraint(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newPointerConstraint);
    self->m_cursor->handleNewConstraint(static_cast<wlr_pointer_constraint_v1*>(data));
  }

  void Server::onNewVirtualPointer(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newVirtualPointer);
    auto* event = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(data);
    auto* vpointer = event->new_pointer;

    auto device = std::make_unique<VirtualPointerDevice>();
    device->server = self;
    device->vpointer = vpointer;
    device->destroy.notify = onVirtualPointerDestroy;
    wl_resource_add_destroy_listener(vpointer->resource, &device->destroy);

    // Attach to the cursor exactly like a physical pointer (see addPointer), so
    // a virtual pointer runs the same Cursor pipeline: hover, click-to-focus,
    // mouse binds, and interactive move and resize. Hand-wiring these signals
    // instead only warped the cursor and forwarded buttons to the seat, so a
    // virtual pointer could move the cursor but never focus or drag anything.
    self->m_cursor->attachInputDevice(&vpointer->pointer.base);

    self->m_virtualPointers.push_back(std::move(device));
  }

  void Server::onVirtualPointerDestroy(wl_listener* listener, void* /*data*/) {
    VirtualPointerDevice* device;
    device = wl_container_of(listener, device, destroy);
    // wlr_cursor detaches the device itself when the pointer is destroyed.
    wl_list_remove(&device->destroy.link);
    std::erase_if(device->server->m_virtualPointers, [device](const std::unique_ptr<VirtualPointerDevice>& ptr) {
      return ptr.get() == device;
    });
  }

  void Server::onNewIdleInhibitor(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newIdleInhibitor);
    auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
    auto* watch = new IdleInhibitorWatch();
    watch->server = self;
    watch->destroy.notify = onIdleInhibitorDestroy;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    self->updateIdleInhibit();
    kLog.debug("idle inhibitor added");
  }

  void Server::onIdleInhibitorDestroy(wl_listener* listener, void* /*data*/) {
    IdleInhibitorWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    delete watch;
    server->updateIdleInhibit();
    kLog.debug("idle inhibitor removed");
  }
  void Server::onPointerDestroy(wl_listener* listener, void* /*data*/) {
    PointerDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_pointers, [watch](const std::unique_ptr<PointerDevice>& pointer) {
      return pointer.get() == watch;
    });
  }

  void Server::onRequestActivate(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_requestActivate);
    auto* event = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
    wlr_xdg_activation_token_v1* token = event->token;
    const char* tokenName = token != nullptr ? wlr_xdg_activation_token_v1_get_name(token) : nullptr;
    const auto* watch = token != nullptr ? static_cast<ActivationTokenWatch*>(token->data) : nullptr;
    const auto age = watch != nullptr
        ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - watch->createdAt)
              .count()
        : -1;
    View* source = token != nullptr ? viewForSurface(*self, token->surface) : nullptr;
    View* target = viewForSurface(*self, event->surface);
    Workspace* targetWorkspace = target != nullptr ? target->workspace() : nullptr;
    const bool targetKeyboardFocused = self->m_seat != nullptr
        && self->m_seat->wlr()->keyboard_state.focused_surface != nullptr
        && wlr_surface_get_root_surface(self->m_seat->wlr()->keyboard_state.focused_surface)
            == wlr_surface_get_root_surface(event->surface);
    const bool targetPointerFocused = self->m_seat != nullptr
        && self->m_seat->wlr()->pointer_state.focused_surface != nullptr
        && wlr_surface_get_root_surface(self->m_seat->wlr()->pointer_state.focused_surface)
            == wlr_surface_get_root_surface(event->surface);
    kLog.debug(
        "xdg-activation activate token='{}' age_ms={} serial={} seat={} source_surface={} source_pid={} "
        "source_app_id='{}' app_id_hint='{}' target_surface={} target_pid={} target_app_id='{}' mapped={} "
        "visible={} keyboard_focused={} pointer_focused={} workspace='{}' workspace_active={} other_workspace={} "
        "locked={}",
        tokenName != nullptr ? tokenName : "<unknown>", age, token != nullptr ? token->serial : 0,
        static_cast<const void*>(token != nullptr ? token->seat : nullptr),
        static_cast<const void*>(token != nullptr ? token->surface : nullptr),
        surfaceClientPid(token != nullptr ? token->surface : nullptr),
        source != nullptr && source->toplevel()->app_id != nullptr ? source->toplevel()->app_id : "",
        token != nullptr && token->app_id != nullptr ? token->app_id : "", static_cast<const void*>(event->surface),
        surfaceClientPid(event->surface),
        target != nullptr && target->toplevel()->app_id != nullptr ? target->toplevel()->app_id : "",
        target != nullptr && target->mapped(), target != nullptr && target->onActiveWorkspace(), targetKeyboardFocused,
        targetPointerFocused, targetWorkspace != nullptr ? targetWorkspace->name() : "",
        targetWorkspace != nullptr && targetWorkspace->active(),
        targetWorkspace != nullptr && !targetWorkspace->active(), self->m_sessionLocked
    );
    if (self->m_sessionLocked) {
      return;
    }

    wlr_surface* root = wlr_surface_get_root_surface(event->surface);
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
    if (toplevel == nullptr) {
      return;
    }

    for (const auto& entry : self->m_registry.all()) {
      if (entry->toplevel() == toplevel && entry->mapped()) {
        const bool focusOnActivate = entry->resolvedRules().focusOnActivate.value_or(config().general.focusOnActivate);
        const bool alreadyFocused = entry->workspace() != nullptr
            && entry->workspace()->active()
            && entry->workspace()->focusedView() == entry.get();
        kLog.debug(
            "xdg-activation policy target_app_id='{}' focus_on_activate={} already_focused={} action={}",
            entry->toplevel()->app_id != nullptr ? entry->toplevel()->app_id : "", focusOnActivate, alreadyFocused,
            alreadyFocused ? "none" : (focusOnActivate ? "focus" : "urgent")
        );
        if (alreadyFocused) {
          entry->setUrgent(false);
        } else if (focusOnActivate) {
          self->focusView(entry.get(), FocusReason::XdgActivation);
        } else {
          entry->setUrgent(true);
        }
        return;
      }
    }
  }

  void Server::onNewActivationToken(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newActivationToken);
    auto* token = static_cast<wlr_xdg_activation_token_v1*>(data);
    auto* watch = new ActivationTokenWatch{
        .createdAt = std::chrono::steady_clock::now(),
    };
    watch->destroy.notify = onActivationTokenDestroy;
    wl_signal_add(&token->events.destroy, &watch->destroy);
    token->data = watch;

    View* source = viewForSurface(*self, token->surface);
    const char* tokenName = wlr_xdg_activation_token_v1_get_name(token);
    kLog.debug(
        "xdg-activation token token='{}' serial={} seat={} source_surface={} source_pid={} source_app_id='{}' "
        "app_id_hint='{}' source_mapped={} source_visible={} source_keyboard_focused={} source_pointer_focused={}",
        tokenName != nullptr ? tokenName : "<unknown>", token->serial, static_cast<const void*>(token->seat),
        static_cast<const void*>(token->surface), surfaceClientPid(token->surface),
        source != nullptr && source->toplevel()->app_id != nullptr ? source->toplevel()->app_id : "",
        token->app_id != nullptr ? token->app_id : "", source != nullptr && source->mapped(),
        source != nullptr && source->onActiveWorkspace(),
        source != nullptr
            && self->m_seat != nullptr
            && self->m_seat->wlr()->keyboard_state.focused_surface != nullptr
            && wlr_surface_get_root_surface(self->m_seat->wlr()->keyboard_state.focused_surface)
                == wlr_surface_get_root_surface(token->surface),
        source != nullptr
            && self->m_seat != nullptr
            && self->m_seat->wlr()->pointer_state.focused_surface != nullptr
            && wlr_surface_get_root_surface(self->m_seat->wlr()->pointer_state.focused_surface)
                == wlr_surface_get_root_surface(token->surface)
    );
  }

  void Server::onActivationTokenDestroy(wl_listener* listener, void* /*data*/) {
    ActivationTokenWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    wl_list_remove(&watch->destroy.link);
    delete watch;
  }

  void Server::onWorkspaceCommit(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_workspaceCommit);
    self->handleWorkspaceCommit(data);
  }

  void Server::onSetGamma(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_setGamma);
    auto* event = static_cast<wlr_gamma_control_manager_v1_set_gamma_event*>(data);
    if (Output* out = self->outputFromWlr(event->output)) {
      out->onGammaChanged(event->control);
    }
  }

  void Server::handleWorkspaceCommit(void* data) {
    auto* event = static_cast<wlr_ext_workspace_v1_commit_event*>(data);
    wlr_ext_workspace_v1_request* request = nullptr;
    wl_list_for_each(request, event->requests, link) {
      switch (request->type) {
      case WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE: {
        WorkspaceGroup* group = workspaceGroupFromHandle(request->create_workspace.group);
        if (group != nullptr) {
          group->createWorkspace(request->create_workspace.name);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->activate.workspace)) {
          workspace->group()->activate(workspace);
          refocus(workspace->group()->output());
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->deactivate.workspace)) {
          workspace->group()->deactivate(workspace);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN:
        // Workspaces stay bound to their output group.
        break;
      case WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE:
        break;
      }
    }
  }

  Workspace* Server::workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<Workspace*>(handle->data);
  }

  WorkspaceGroup* Server::workspaceGroupFromHandle(wlr_ext_workspace_group_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<WorkspaceGroup*>(handle->data);
  }

  void Server::updateIdleInhibit() {
    const bool inhibited = !wl_list_empty(&m_idleInhibitManager->inhibitors);
    wlr_idle_notifier_v1_set_inhibited(m_idleNotifier, inhibited);
  }

  void Server::notifyIdleActivity() { wlr_idle_notifier_v1_notify_activity(m_idleNotifier, m_seat->wlr()); }

  void Server::beginSessionLock(wlr_session_lock_v1* lock) {
    if (m_sessionLock != nullptr) {
      kLog.info("denying session lock; one is already active");
      wlr_session_lock_v1_destroy(lock);
      return;
    }

    cancelModifierTap();
    m_sessionLocked = true;
    m_overview->forceClose();
    if (m_cheatsheet != nullptr) {
      m_cheatsheet->hide();
    }
    m_cursor->resetMode();
    m_cursor->clearConstraint();
    clearNormalFocus();
    updateLockBlank();
    setLockBlankEnabled(true);
    raiseLockTree();
    m_sessionLock = std::make_unique<SessionLock>(*this, lock);
  }

  void Server::unlockSession() {
    m_sessionLocked = false;
    wlr_scene_node_set_enabled(&m_lockBlank->node, false);
    if (View* recent = m_registry.mostRecent()) {
      focusView(recent);
    }
  }

  void Server::removeSessionLock(SessionLock* lock) {
    if (m_sessionLock.get() != lock) {
      return;
    }
    m_sessionLock.reset();
    if (!m_sessionLocked) {
      wlr_scene_node_set_enabled(&m_lockTree->node, false);
    }
  }

  void Server::setLockBlankEnabled(bool enabled) {
    wlr_scene_node_set_enabled(&m_lockTree->node, enabled);
    wlr_scene_node_set_enabled(&m_lockBlank->node, enabled);
    if (enabled) {
      raiseLockTree();
    }
  }

  // Security boundary: never defer this through Dirty. One stale frame can
  // expose content that the lock exists to hide.
  void Server::updateLockBlank() {
    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_outputLayout, nullptr, &layoutBox);
    if (layoutBox.width <= 0 || layoutBox.height <= 0) {
      return;
    }
    wlr_scene_rect_set_color(m_lockBlank, config().appearance.backdropColor.data());
    wlr_scene_rect_set_size(m_lockBlank, layoutBox.width, layoutBox.height);
    wlr_scene_node_set_position(&m_lockBlank->node, layoutBox.x, layoutBox.y);
  }

  void Server::updateBackdrop() {
    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_outputLayout, nullptr, &layoutBox);
    if (layoutBox.width <= 0 || layoutBox.height <= 0) {
      return;
    }
    wlr_scene_rect_set_color(m_backdrop, config().appearance.backdropColor.data());
    wlr_scene_rect_set_size(m_backdrop, layoutBox.width, layoutBox.height);
    wlr_scene_node_set_position(&m_backdrop->node, layoutBox.x, layoutBox.y);
    for (const auto& output : m_outputs) {
      output->markBlurBackgroundDirty();
    }
  }

  void Server::raiseLockTree() { wlr_scene_node_raise_to_top(&m_lockTree->node); }

  void Server::addOutput(wlr_output* output) {
    m_outputs.push_back(std::make_unique<Output>(*this, output));
    markDirty(Dirty::Backdrop | Dirty::Banner | Dirty::Cheatsheet);
    if (m_sessionLocked) {
      updateLockBlank();
      raiseLockTree();
    }
    updateOutputManagerConfig();
  }

  void Server::addKeyboard(wlr_input_device* device) {
    m_keyboards.push_back(std::make_unique<Keyboard>(*this, device));
  }

  void Server::addPointer(wlr_input_device* device) {
    auto pointer = std::make_unique<PointerDevice>();
    pointer->server = this;
    pointer->device = device;
    pointer->destroy.notify = onPointerDestroy;
    wl_signal_add(&device->events.destroy, &pointer->destroy);
    applyPointerConfig(device);
    m_cursor->attachInputDevice(device);
    m_pointers.push_back(std::move(pointer));
  }

  void Server::addTouch(wlr_input_device* device) {
    auto touch = std::make_unique<TouchDevice>();
    touch->server = this;
    touch->device = device;
    touch->destroy.notify = onTouchDestroy;
    wl_signal_add(&device->events.destroy, &touch->destroy);
    m_cursor->attachInputDevice(device);
    m_touchDevices.push_back(std::move(touch));
    kLog.info("input: added touch device '{}'", deviceName(device));
  }

  void Server::onTouchDestroy(wl_listener* listener, void* /*data*/) {
    TouchDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_touchDevices, [watch](const std::unique_ptr<TouchDevice>& entry) {
      return entry.get() == watch;
    });
    server->updateSeatCapabilities();
  }

  void Server::removeOutput(Output* output) {
    m_overview->onOutputRemoved(output);
    m_gestures->cancelForOutput(output);
    if (!m_cursor->isPassthrough()) {
      m_cursor->resetMode();
    }
    if (m_insertHint != nullptr && m_insertHint->output() == output) {
      m_insertHint->hideImmediate();
    }
    // Dropping the snapshot unregisters it and destroys its scene tree.
    std::erase_if(m_closeSnapshots, [this, output](const std::unique_ptr<CloseSnapshot>& snap) {
      if (!snap->animatesOn(output)) {
        return false;
      }
      unregisterAnimatable(snap.get());
      return true;
    });
    // wlroots 0.20 does not track output lifetime for layer surfaces, so
    // their wlr_output pointer would dangle once the output is freed.
    // Destroy them now while the wlr_output is still valid.
    {
      wlr_output* dying = output->wlr();
      std::vector<wlr_layer_surface_v1*> toClose;
      for (const auto& ls : m_layerSurfaces) {
        if (ls->layerSurface() != nullptr && ls->layerSurface()->output == dying) {
          toClose.push_back(ls->layerSurface());
        }
      }
      for (wlr_layer_surface_v1* ls : toClose) {
        wlr_layer_surface_v1_destroy(ls);
      }
    }

    WorkspaceGroup* dying = output->workspaceGroup();
    Output* fallback = nullptr;
    for (const auto& entry : m_outputs) {
      if (entry.get() != output) {
        fallback = entry.get();
        break;
      }
    }

    if (dying != nullptr) {
      Workspace* destination = nullptr;
      if (fallback != nullptr && fallback->workspaceGroup() != nullptr) {
        destination = fallback->workspaceGroup()->active();
      }
      for (const auto& entry : m_registry.all()) {
        Workspace* workspace = entry->workspace();
        if (workspace == nullptr || workspace->group() != dying) {
          continue;
        }
        entry->setWorkspace(destination);
      }
    }
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->moveOutput(output, fallback);
    }

    std::erase_if(m_outputs, [output](const std::unique_ptr<Output>& entry) { return entry.get() == output; });
    markDirty(Dirty::Banner | Dirty::Cheatsheet);
    if (m_sessionLocked) {
      updateLockBlank();
    }
    updateOutputManagerConfig();
  }

  void Server::removeKeyboard(Keyboard* keyboard) {
    std::erase_if(m_keyboards, [keyboard](const std::unique_ptr<Keyboard>& entry) { return entry.get() == keyboard; });
    updateSeatCapabilities();
  }

  void Server::removeView(View* view) {
    if (view == nullptr) {
      return;
    }
    const bool hadKeyboardFocus = m_seat->wlr()->keyboard_state.focused_surface == view->toplevel()->base->surface;
    if (m_scratchpadManager != nullptr) {
      m_scratchpadManager->remove(view);
    }
    View* replacement = nullptr;
    Output* output = nullptr;
    if (Workspace* workspace = view->workspace()) {
      if (workspace->group() != nullptr) {
        output = workspace->group()->output();
      }
      replacement = workspace->removeView(view);
      view->detachWorkspace();
    }
    m_registry.remove(view);
    if (hadKeyboardFocus) {
      wlr_seat_keyboard_notify_clear_focus(m_seat->wlr());
      if (replacement != nullptr) {
        focusView(replacement);
      } else {
        refocus(output);
      }
    }
  }

  void Server::removeLayerSurface(LayerSurface* layerSurface, wlr_output* output) {
    std::erase_if(m_layerSurfaces, [layerSurface](const std::unique_ptr<LayerSurface>& entry) {
      return entry.get() == layerSurface;
    });
    arrangeLayers(output);
  }

  void Server::onOutputManagerApply(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_outputManagerApply);
    self->applyOutputManagerConfig(static_cast<wlr_output_configuration_v1*>(data), false);
  }

  void Server::onOutputManagerTest(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_outputManagerTest);
    self->applyOutputManagerConfig(static_cast<wlr_output_configuration_v1*>(data), true);
  }

  void Server::onOutputLayoutChange(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_outputLayoutChange);
    self->markDirty(Dirty::Backdrop);
    self->updateOutputManagerConfig();
  }

  void Server::updateOutputManagerConfig() {
    if (m_outputManager == nullptr) {
      return;
    }
    wlr_output_configuration_v1* cfg = wlr_output_configuration_v1_create();
    for (const auto& output : m_outputs) {
      wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(cfg, output->wlr());
      if (wlr_output_layout_output* lo = wlr_output_layout_get(m_outputLayout, output->wlr())) {
        head->state.x = lo->x;
        head->state.y = lo->y;
      }
    }
    wlr_output_manager_v1_set_configuration(m_outputManager, cfg);
  }

  void Server::applyOutputManagerConfig(wlr_output_configuration_v1* config, bool testOnly) {
    // Reject disabling outputs: umbriel has no disabled-output state model.
    wlr_output_configuration_head_v1* head = nullptr;
    wl_list_for_each(head, &config->heads, link) {
      if (!head->state.enabled) {
        kLog.warn("output-management: disabling outputs is not supported");
        wlr_output_configuration_v1_send_failed(config);
        wlr_output_configuration_v1_destroy(config);
        return;
      }
    }

    size_t statesLen = 0;
    wlr_backend_output_state* states = wlr_output_configuration_v1_build_state(config, &statesLen);
    if (states == nullptr) {
      wlr_output_configuration_v1_send_failed(config);
      wlr_output_configuration_v1_destroy(config);
      return;
    }

    bool ok = wlr_backend_test(m_backend, states, statesLen);
    if (ok && !testOnly) {
      ok = wlr_backend_commit(m_backend, states, statesLen);
    }

    if (ok && !testOnly) {
      // Apply layout positions and refresh affected outputs.
      wl_list_for_each(head, &config->heads, link) {
        wlr_output_layout_add(m_outputLayout, head->state.output, head->state.x, head->state.y);
        if (Output* out = outputFromWlr(head->state.output)) {
          out->handleExternalConfigChange();
        }
      }
      markDirty(Dirty::Banner | Dirty::Cheatsheet);
      if (m_sessionLocked) {
        updateLockBlank();
      }
    }

    free(states);
    if (ok) {
      wlr_output_configuration_v1_send_succeeded(config);
    } else {
      wlr_output_configuration_v1_send_failed(config);
    }
    wlr_output_configuration_v1_destroy(config);

    if (ok && !testOnly) {
      updateOutputManagerConfig();
    }
  }

  void Server::onToplevelCaptureRequest(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_toplevelCaptureRequest);
    auto* request = static_cast<wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request*>(data);

    if (request->toplevel_handle == nullptr || request->toplevel_handle->data == nullptr) {
      return;
    }
    auto* view = static_cast<View*>(request->toplevel_handle->data);

    // Reuse a cached capture source when one already exists for this view.
    if (view->m_captureSource == nullptr) {
      view->m_captureSource = wlr_ext_image_capture_source_v1_create_with_scene_node(
          &view->sceneTree()->node, wl_display_get_event_loop(self->m_display), self->m_allocator, self->m_renderer
      );
      if (view->m_captureSource == nullptr) {
        return;
      }
      view->m_captureSourceDestroy.notify = View::onCaptureSourceDestroy;
      wl_signal_add(&view->m_captureSource->events.destroy, &view->m_captureSourceDestroy);
    }

    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, view->m_captureSource);
  }
} // namespace umbriel
