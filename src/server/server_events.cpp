#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
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

  void Server::onNewOutput(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_newOutput);
    self->addOutput(static_cast<wlr_output*>(data));
  }

  void Server::onNewInput(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_newInput);
    auto* device = static_cast<wlr_input_device*>(data);
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
      self->addKeyboard(device);
      break;
    case WLR_INPUT_DEVICE_POINTER:
      self->addPointer(device);
      break;
    default:
      break;
    }
    self->updateSeatCapabilities();
  }

  void Server::onNewXdgToplevel(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_newXdgToplevel);
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

  void Server::onNewLayerSurface(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_newLayerSurface);
    auto surface = std::make_unique<LayerSurface>(*self, static_cast<wlr_layer_surface_v1*>(data));
    if (surface->layerSurface() == nullptr) {
      return;
    }
    self->m_layerSurfaces.push_back(std::move(surface));
  }

  void Server::onNewSessionLock(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_newSessionLock);
    self->beginSessionLock(static_cast<wlr_session_lock_v1*>(data));
  }

  void Server::onNewPointerConstraint(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_newPointerConstraint);
    self->m_cursor->handleNewConstraint(static_cast<wlr_pointer_constraint_v1*>(data));
  }

  void Server::onNewIdleInhibitor(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_newIdleInhibitor);
    auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
    auto* watch = new IdleInhibitorWatch();
    watch->server = self;
    watch->destroy.notify = onIdleInhibitorDestroy;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    self->updateIdleInhibit();
    kLog.debug("idle inhibitor added");
  }

  void Server::onIdleInhibitorDestroy(wl_listener* listener, void* /*data*/) {
    IdleInhibitorWatch* watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    delete watch;
    server->updateIdleInhibit();
    kLog.debug("idle inhibitor removed");
  }

  void Server::onRequestActivate(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_requestActivate);
    auto* event = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
    if (self->m_sessionLocked || self->exclusiveKeyboardLayer() != nullptr) {
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
    Server* self = wl_container_of(listener, self, m_workspaceCommit);
    self->handleWorkspaceCommit(data);
  }

  void Server::onSetGamma(wl_listener* listener, void* data) {
    Server* self = wl_container_of(listener, self, m_setGamma);
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
          refocus();
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
    if (m_sessionLocked) {
      updateLockBlank();
      raiseLockTree();
    }
  }

  void Server::addKeyboard(wlr_input_device* device) {
    m_keyboards.push_back(std::make_unique<Keyboard>(*this, device));
  }

  void Server::addPointer(wlr_input_device* device) {
    applyPointerConfig(device);
    m_cursor->attachInputDevice(device);
  }

  void Server::removeOutput(Output* output) {
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
    if (m_sessionLocked) {
      updateLockBlank();
    }
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
    if (Workspace* workspace = view->workspace()) {
      replacement = workspace->removeView(view);
      view->detachWorkspace();
    }
    std::erase_if(m_views, [view](const std::unique_ptr<View>& entry) { return entry.get() == view; });
    if (hadKeyboardFocus) {
      if (replacement != nullptr) {
        focusView(replacement);
      } else {
        refocus();
      }
    }
  }

  void Server::removeLayerSurface(LayerSurface* layerSurface, wlr_output* output) {
    std::erase_if(m_layerSurfaces, [layerSurface](const std::unique_ptr<LayerSurface>& entry) {
      return entry.get() == layerSurface;
    });
    arrangeLayers(output);
  }

} // namespace umbriel
