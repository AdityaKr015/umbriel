#include "output/output.h"

#include "config/config.h"
#include "core/log.h"
#include "layer/layer_surface.h"
#include "scene/node.h"
#include "server/server.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <cstdlib>
#include <ctime>

namespace umbriel {

  namespace {
    constexpr Logger kLog("output");

    const OutputRule* findRule(const char* name) {
      for (const OutputRule& rule : config().outputs) {
        if (rule.name == name) {
          return &rule;
        }
      }
      return nullptr;
    }
  } // namespace

  Output::Output(Server& server, wlr_output* output) : m_server(&server), m_output(output) {
    m_output->data = this;
    wlr_output_init_render(m_output, m_server->allocator(), m_server->renderer());

    m_frame.notify = onFrame;
    wl_signal_add(&m_output->events.frame, &m_frame);

    m_requestState.notify = onRequestState;
    wl_signal_add(&m_output->events.request_state, &m_requestState);

    m_destroy.notify = onDestroy;
    wl_signal_add(&m_output->events.destroy, &m_destroy);

    applyConfiguredState();
    wlr_output_layout_output* layoutOutput = addToLayout();
    m_sceneOutput = wlr_scene_output_create(m_server->scene(), m_output);
    wlr_scene_output_layout_add_output(m_server->sceneLayout(), layoutOutput, m_sceneOutput);

    for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      m_layerTrees[layer] = wlr_scene_tree_create(m_server->shellLayerTree(layer));
    }
    m_popupTree = wlr_scene_tree_create(m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY));
    arrangeLayers();
    m_workspaceGroup = std::make_unique<WorkspaceGroup>(*m_server, *this);
  }
  void Output::applyConfiguredState() {
    const OutputRule* rule = findRule(m_output->name);
    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    if (rule != nullptr && rule->mode) {
      if (wlr_output_is_wl(m_output)) {
        kLog.info("output '{}': mode is ignored in nested sessions", m_output->name);
      } else {
        const OutputMode& configured = *rule->mode;
        wlr_output_mode* selected = nullptr;
        wlr_output_mode* mode = nullptr;
        wl_list_for_each(mode, &m_output->modes, link) {
          if (mode->width != configured.width || mode->height != configured.height) {
            continue;
          }
          if (configured.refreshMHz != 0) {
            if (selected == nullptr
                || std::abs(mode->refresh - configured.refreshMHz)
                    < std::abs(selected->refresh - configured.refreshMHz)) {
              selected = mode;
            }
          } else if (
              selected == nullptr
              || (mode->preferred && !selected->preferred)
              || (mode->preferred == selected->preferred && mode->refresh > selected->refresh)
          ) {
            selected = mode;
          }
        }
        if (selected != nullptr) {
          wlr_output_state_set_mode(&state, selected);
        } else {
          wlr_output_state_set_custom_mode(&state, configured.width, configured.height, configured.refreshMHz);
        }
      }
    } else if (!wlr_output_is_wl(m_output)) {
      if (wlr_output_mode* mode = wlr_output_preferred_mode(m_output)) {
        wlr_output_state_set_mode(&state, mode);
      }
    }

    if (rule != nullptr && rule->scale) {
      wlr_output_state_set_scale(&state, static_cast<float>(*rule->scale));
    }
    if (rule != nullptr && rule->transform) {
      wlr_output_state_set_transform(&state, static_cast<wl_output_transform>(*rule->transform));
    }

    const bool committed = wlr_output_commit_state(m_output, &state);
    wlr_output_state_finish(&state);
    if (!committed) {
      kLog.error("output '{}': failed to commit configured state", m_output->name);
      return;
    }
    kLog.info(
        "output '{}': applied mode={}x{}@{}mHz scale={} transform={}", m_output->name, m_output->width,
        m_output->height, m_output->refresh, m_output->scale, static_cast<int>(m_output->transform)
    );
  }

  wlr_output_layout_output* Output::addToLayout() {
    const OutputRule* rule = findRule(m_output->name);
    if (rule != nullptr && rule->position) {
      return wlr_output_layout_add(m_server->outputLayout(), m_output, (*rule->position)[0], (*rule->position)[1]);
    }
    return wlr_output_layout_add_auto(m_server->outputLayout(), m_output);
  }

  void Output::applyOutputState() {
    applyConfiguredState();
    addToLayout();
    markDirty(Dirty::LayerArrange | Dirty::Banner);
    if (m_server->sessionLocked()) {
      m_server->updateLockBlank();
    }
    wlr_output_schedule_frame(m_output);
  }

  void Output::handleExternalConfigChange() {
    // Mode changes can drop the DRM gamma LUT; re-apply on the next frame.
    m_gammaDirty = true;
    markDirty(Dirty::LayerArrange);
    wlr_output_schedule_frame(m_output);
  }

  Output::~Output() {
    if (m_output != nullptr && m_output->data == this) {
      m_output->data = nullptr;
    }
    if (m_frame.link.next != nullptr) {
      wl_list_remove(&m_frame.link);
      wl_list_remove(&m_requestState.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  wlr_scene_tree* Output::layerTree(uint32_t layer) const {
    if (layer >= kLayerCount) {
      return m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP];
    }
    return m_layerTrees[layer];
  }

  void Output::arrangeLayer(wlr_scene_tree* tree, const wlr_box* fullArea, wlr_box* usableArea, bool exclusive) {
    wlr_scene_node* node = nullptr;
    wl_list_for_each(node, &tree->children, link) {
      SceneNode* sceneNode = sceneNodeFrom(node->data);
      if (sceneNode == nullptr || sceneNode->kind != SceneNodeKind::LayerSurface) {
        continue;
      }
      auto* layerSurface = static_cast<LayerSurface*>(sceneNode);
      if (layerSurface->scene() == nullptr || layerSurface->arrangingOut()) {
        continue;
      }
      wlr_layer_surface_v1* surface = layerSurface->layerSurface();
      if (surface == nullptr || !surface->initialized) {
        continue;
      }
      // Match sway: only exclusive_zone > 0 participates in the exclusive pass.
      if ((surface->current.exclusive_zone > 0) != exclusive) {
        continue;
      }
      wlr_scene_layer_surface_v1_configure(layerSurface->scene(), fullArea, usableArea);
    }
  }

  void Output::arrangeLayers() {
    wlr_box fullArea{};
    wlr_output_effective_resolution(m_output, &fullArea.width, &fullArea.height);
    if (fullArea.width <= 0 || fullArea.height <= 0) {
      return;
    }

    wlr_box usableArea = fullArea;

    // Exclusive first, overlay down to background so higher layers win the zone.
    static constexpr uint32_t kExclusiveOrder[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };
    for (uint32_t layer : kExclusiveOrder) {
      arrangeLayer(m_layerTrees[layer], &fullArea, &usableArea, true);
    }
    for (uint32_t layer : kExclusiveOrder) {
      arrangeLayer(m_layerTrees[layer], &fullArea, &usableArea, false);
    }
    updateOptimizedBlur(fullArea);

    // Layer trees are output-local; pin them to the scene-output origin.
    for (auto& m_layerTree : m_layerTrees) {
      wlr_scene_node_set_position(&m_layerTree->node, m_sceneOutput->x, m_sceneOutput->y);
    }
    wlr_scene_node_set_position(&m_popupTree->node, m_sceneOutput->x, m_sceneOutput->y);

    // Usable area is stored in layout coordinates for xdg placement.
    m_usableArea = {
        .x = m_sceneOutput->x + usableArea.x,
        .y = m_sceneOutput->y + usableArea.y,
        .width = usableArea.width,
        .height = usableArea.height,
    };

    kLog.debug(
        "{} usable={}x{}+{}+{}", m_output->name, m_usableArea.width, m_usableArea.height, m_usableArea.x, m_usableArea.y
    );
    if (m_workspaceGroup != nullptr && m_workspaceGroup->active() != nullptr) {
      m_workspaceGroup->active()->markArrange(false);
    }
  }

  void Output::updateOptimizedBlur(const wlr_box& fullArea) {
    const auto& blur = config().appearance.blur;
    if (!blur.enabled || !blur.optimized) {
      if (m_optimizedBlur != nullptr) {
        wlr_scene_node_destroy(&m_optimizedBlur->node);
        m_optimizedBlur = nullptr;
      }
      return;
    }

    if (m_optimizedBlur == nullptr) {
      m_optimizedBlur = wlr_scene_optimized_blur_create(
          m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND), fullArea.width, fullArea.height
      );
      if (m_optimizedBlur == nullptr) {
        return;
      }
    }

    const bool changed = m_optimizedBlur->node.x != m_sceneOutput->x
        || m_optimizedBlur->node.y != m_sceneOutput->y
        || m_optimizedBlur->width != fullArea.width
        || m_optimizedBlur->height != fullArea.height;
    wlr_scene_node_set_enabled(&m_optimizedBlur->node, true);
    wlr_scene_node_set_position(&m_optimizedBlur->node, m_sceneOutput->x, m_sceneOutput->y);
    wlr_scene_optimized_blur_set_size(m_optimizedBlur, fullArea.width, fullArea.height);
    if (changed) {
      wlr_scene_optimized_blur_mark_dirty(m_optimizedBlur);
    }
  }

  void Output::markBlurBackgroundDirty() {
    if (m_optimizedBlur != nullptr) {
      wlr_scene_optimized_blur_mark_dirty(m_optimizedBlur);
    }
  }

  void Output::onGammaChanged(wlr_gamma_control_v1* /*control*/) {
    // DRM gamma LUT upload is expensive; apply once on change, not every frame.
    m_gammaDirty = true;
    wlr_output_schedule_frame(m_output);
  }

  void Output::onFrame(wl_listener* listener, void* /*data*/) {
    Output* self;
    self = wl_container_of(listener, self, m_frame);
    self->handleFrame();
  }

  void Output::onRequestState(wl_listener* listener, void* data) {
    Output* self;
    self = wl_container_of(listener, self, m_requestState);
    self->handleRequestState(data);
  }

  void Output::onDestroy(wl_listener* listener, void* /*data*/) {
    Output* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void Output::applyMode(int width, int height) {
    if (width <= 0 || height <= 0) {
      return;
    }

    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, width, height, 0);
    if (!wlr_output_commit_state(m_output, &state)) {
      wlr_log(WLR_ERROR, "failed to commit output mode %dx%d for '%s'", width, height, m_output->name);
    } else {
      // Mode changes can drop the DRM gamma LUT; re-apply on the next frame.
      m_gammaDirty = true;
    }
    wlr_output_state_finish(&state);
    markDirty(Dirty::LayerArrange | Dirty::Banner | Dirty::Backdrop);
    if (m_server->sessionLocked()) {
      m_server->updateLockBlank();
    }
    wlr_output_schedule_frame(m_output);
  }

  void Output::markDirty(Dirty what) {
    m_dirty |= what;
    wlr_output_schedule_frame(m_output);
  }

  void Output::flushDirty() {
    // Server-wide chrome is recorded on the Server and flushed by whichever
    // output frames first; each of these is idempotent and cheap.
    Dirty pending = m_dirty | m_server->takeDirty();
    m_dirty = Dirty::None;
    // Order matters: exclusive zones define the usable area, the layout fills
    // it, and the chrome sits over the result.
    if (has(pending, Dirty::LayerArrange)) {
      arrangeLayers();
      // Changing the usable area makes the layout stale, so arrangeLayers marks
      // it, after this set was taken. Take again rather than let that wait a
      // frame; the same holds for anything a later step records for a step
      // further down.
      pending |= m_dirty;
      m_dirty = Dirty::None;
    }
    if (has(pending, Dirty::Layout) && m_workspaceGroup != nullptr) {
      m_workspaceGroup->flushArrange();
    }
    if (has(pending, Dirty::Banner)) {
      m_server->relayoutBanner();
    }
    if (has(pending, Dirty::Backdrop)) {
      m_server->updateBackdrop();
    }
    if (has(pending, Dirty::Cheatsheet)) {
      m_server->relayoutCheatsheet();
    }
    if (has(pending, Dirty::QuitConfirm)) {
      m_server->relayoutQuitConfirm();
    }
  }

  void Output::handleFrame() {
    flushDirty();
    if (m_hasDeferredMode) {
      m_hasDeferredMode = false;
      applyMode(m_deferredWidth, m_deferredHeight);
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t nowMsec = static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1'000'000;
    m_server->tickAnimations(nowMsec);

    if (m_output->width <= 0 || m_output->height <= 0) {
      // Output not configured yet; no clients can be presenting on it either.
      return;
    }

    // Render + commit only if the scene actually changed or a gamma upload is pending.
    // All exit paths below MUST reach the unconditional wlr_scene_output_send_frame_done
    // call at the bottom: mailbox/FIFO clients (games via DXVK, video players) block on
    // wl_surface.frame before submitting their next buffer. If we skip frame_done on the
    // "nothing to render" path, they never commit again -> damage stays clean ->
    // wlr_scene_output_needs_frame returns false forever -> compositor parks in epoll_wait.
    // (Reproducible with any mailbox/FIFO Vulkan game.)
    if (wlr_scene_output_needs_frame(m_sceneOutput) || m_gammaDirty) {
      m_inFrame = true;

      wlr_output_state state{};
      wlr_output_state_init(&state);

      bool commitOk = false;
      if (wlr_scene_output_build_state(m_sceneOutput, &state, nullptr)) {
        // Hardware gamma only (DRM). Nested Wayland has no gamma LUT; leave that alone.
        // Apply only when dirty: uploading the LUT every frame stalls the compositor.
        bool gammaPending = false;
        if (m_gammaDirty) {
          if (wlr_output_get_gamma_size(m_output) > 0) {
            wlr_gamma_control_v1* control =
                wlr_gamma_control_manager_v1_get_control(m_server->gammaManager(), m_output);
            if (!wlr_gamma_control_v1_apply(control, &state)) {
              if (control != nullptr) {
                wlr_gamma_control_v1_send_failed_and_destroy(control);
              }
              m_gammaDirty = false;
            } else {
              gammaPending = true;
            }
          } else {
            m_gammaDirty = false;
          }
        }

        commitOk = wlr_output_commit_state(m_output, &state);
        if (commitOk && gammaPending) {
          m_gammaDirty = false;
        }
      }

      wlr_output_state_finish(&state);
      m_inFrame = false;

      if (!commitOk) {
        // Retry on next vblank; scene may have changed or backend may have recovered.
        wlr_output_schedule_frame(m_output);
      }
    }

    // A request_state that arrived mid-commit is applied now that we're out of it.
    if (m_hasDeferredMode) {
      m_hasDeferredMode = false;
      applyMode(m_deferredWidth, m_deferredHeight);
    }

    // Keep this output ticking on the next vblank while it owns an animation.
    if (m_server->animationsActiveFor(this)) {
      wlr_output_schedule_frame(m_output);
    }

    // Unconditional: see comment above. Never gate this on commit success.
    wlr_scene_output_send_frame_done(m_sceneOutput, &now);
  }

  void Output::handleRequestState(void* data) {
    auto* event = static_cast<wlr_output_event_request_state*>(data);

    // Parent configure can arrive while we are flushing a frame commit. Applying a
    // mode change mid-frame makes the wayland backend reject the primary buffer.
    if (m_inFrame
        && (event->state->committed & WLR_OUTPUT_STATE_MODE) != 0
        && event->state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
      m_deferredWidth = event->state->custom_mode.width;
      m_deferredHeight = event->state->custom_mode.height;
      m_hasDeferredMode = true;
      return;
    }

    if (!wlr_output_commit_state(m_output, event->state)) {
      wlr_log(WLR_ERROR, "failed to commit requested output state for '%s'", m_output->name);
      return;
    }
    if ((event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ENABLED)) != 0) {
      markDirty(Dirty::LayerArrange | Dirty::Banner | Dirty::Backdrop);
      m_gammaDirty = true;
    }
    wlr_output_schedule_frame(m_output);
  }

  void Output::handleDestroy() {
    if (m_output->data == this) {
      m_output->data = nullptr;
    }
    wl_list_remove(&m_frame.link);
    wl_list_remove(&m_requestState.link);
    wl_list_remove(&m_destroy.link);
    m_frame.link.next = nullptr;
    m_requestState.link.next = nullptr;
    m_destroy.link.next = nullptr;
    if (m_optimizedBlur != nullptr && m_server->scene() != nullptr) {
      wlr_scene_node_destroy(&m_optimizedBlur->node);
    }
    m_optimizedBlur = nullptr;
    m_server->removeOutput(this);
  }

} // namespace umbriel
