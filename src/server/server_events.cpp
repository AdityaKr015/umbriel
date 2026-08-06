#include "config/config.h"
#include "config/config_diag.h"
#include "config/config_watcher.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/gestures.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "layer/surface.h"
#include "lock/session_lock.h"
#include "output/output.h"
#include "server/server.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("server");

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
        libinput_device* libinputDevice, const wlr_input_device* device, const std::optional<bool>& enabled,
        std::string_view setting
    ) {
      if (!enabled) {
        return;
      }
      if (libinput_device_config_scroll_has_natural_scroll(libinputDevice) == 0) {
        kLog.warn("input: '{}' does not support {}", deviceName(device), setting);
        return;
      }
      if (libinput_device_config_scroll_set_natural_scroll_enabled(libinputDevice, *enabled)
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

      const bool isTouchpad = libinput_device_config_tap_get_finger_count(libinputDevice) > 0;
      if (isTouchpad) {
        const Config::Input::Touchpad& touchpad = config().input.touchpad;
        if (touchpad.tap) {
          const auto state = *touchpad.tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED;
          if (libinput_device_config_tap_set_enabled(libinputDevice, state) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
            kLog.warn("input: failed to apply input.touchpad.tap to '{}'", deviceName(device));
          }
        }
        applyNaturalScroll(libinputDevice, device, touchpad.naturalScroll, "input.touchpad.natural_scroll");
        return;
      }

      applyNaturalScroll(libinputDevice, device, config().input.mouse.naturalScroll, "input.mouse.natural_scroll");
    }
  } // namespace
  void Server::applyConfig() {
    const Config::Appearance::Blur& blur = config().appearance.blur;
    wlr_scene_set_blur_data(
        m_scene, blur.passes, blur.radius, static_cast<float>(blur.noise), static_cast<float>(blur.brightness),
        static_cast<float>(blur.contrast), static_cast<float>(blur.saturation)
    );
    for (const auto& keyboard : m_keyboards) {
      keyboard->applyConfig();
    }
    for (const auto& pointer : m_pointers) {
      applyPointerConfig(pointer->device);
    }
    m_cursor->applyConfig();
    for (const auto& output : m_outputs) {
      output->applyConfig();
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        for (size_t i = 0; i < group->workspaceCount(); ++i) {
          if (Workspace* workspace = group->workspaceAt(i)) {
            workspace->recreateLayout();
          }
        }
      }
    }
    for (const auto& view : m_views) {
      if (!view->mapped()) {
        continue;
      }
      view->setBorderFocused(false);
      view->updateBorderGeometry();
      view->applyCornerRadius();
      view->updateBlur();
      view->updateShadow();
    }
    for (const auto& layer : m_layerSurfaces) {
      if (layer->mapped()) {
        layer->updateBlur();
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
    refocus();
    if (m_sessionLocked) {
      updateLockBlank();
    }
    updateOutputManagerConfig();
  }

  void Server::handleConfigReload() {
    if (reloadConfig()) {
      applyConfig();
      kLog.info("config reloaded");
    }
    showConfigDiagnostics();
    if (m_configWatcher != nullptr) {
      m_configWatcher->watch(configWatchPaths());
    }
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
    self->m_views.push_back(std::make_unique<View>(*self, static_cast<wlr_xdg_toplevel*>(data)));
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
    if (self->m_sessionLocked) {
      return;
    }

    wlr_surface* root = wlr_surface_get_root_surface(event->surface);
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
    if (toplevel == nullptr) {
      return;
    }

    for (const auto& entry : self->m_views) {
      if (entry->toplevel() == toplevel && entry->mapped()) {
        kLog.debug("xdg-activation focus app_id='{}'", toplevel->app_id != nullptr ? toplevel->app_id : "");
        self->focusView(entry.get());
        return;
      }
    }
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

    m_sessionLocked = true;
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
    if (!m_views.empty()) {
      focusView(m_views.front().get());
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

  void Server::clearNormalFocus() {
    wlr_seat* seat = m_seat->wlr();
    wlr_seat_keyboard_notify_clear_focus(seat);
    wlr_seat_pointer_clear_focus(seat);
    for (const auto& entry : m_views) {
      if (entry->mapped()) {
        wlr_xdg_toplevel_set_activated(entry->toplevel(), false);
        entry->setBorderFocused(false);
        entry->setForeignActivated(false);
      }
    }
  }

  void Server::setLockBlankEnabled(bool enabled) {
    wlr_scene_node_set_enabled(&m_lockTree->node, enabled);
    wlr_scene_node_set_enabled(&m_lockBlank->node, enabled);
    if (enabled) {
      raiseLockTree();
    }
  }

  void Server::updateLockBlank() {
    wlr_box layoutBox{};
    wlr_output_layout_get_box(m_outputLayout, nullptr, &layoutBox);
    if (layoutBox.width <= 0 || layoutBox.height <= 0) {
      return;
    }
    wlr_scene_rect_set_size(m_lockBlank, layoutBox.width, layoutBox.height);
    wlr_scene_node_set_position(&m_lockBlank->node, layoutBox.x, layoutBox.y);
  }

  void Server::raiseLockTree() { wlr_scene_node_raise_to_top(&m_lockTree->node); }

  void Server::addOutput(wlr_output* output) {
    m_outputs.push_back(std::make_unique<Output>(*this, output));
    relayoutBanner();
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
    m_gestures->cancelForOutput(output);
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
      for (const auto& entry : m_views) {
        Workspace* workspace = entry->workspace();
        if (workspace == nullptr || workspace->group() != dying) {
          continue;
        }
        entry->setWorkspace(destination);
      }
    }

    std::erase_if(m_outputs, [output](const std::unique_ptr<Output>& entry) { return entry.get() == output; });
    relayoutBanner();
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
    View* replacement = nullptr;
    Output* output = nullptr;
    if (Workspace* workspace = view->workspace()) {
      if (workspace->group() != nullptr) {
        output = workspace->group()->output();
      }
      replacement = workspace->removeView(view);
      view->detachWorkspace();
    }
    std::erase_if(m_views, [view](const std::unique_ptr<View>& entry) { return entry.get() == view; });
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
      relayoutBanner();
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
