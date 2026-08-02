#include "output/output.h"

#include "core/log.h"
#include "layer/surface.h"
#include "scene/node.h"
#include "server/server.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <ctime>

namespace umbriel {

  namespace {
    constexpr Logger kLog("output");
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

    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    // Nested Wayland outputs get their real size from the parent configure
    // (request_state). Setting preferred mode here races that path.
    if (!wlr_output_is_wl(m_output)) {
      if (wlr_output_mode* mode = wlr_output_preferred_mode(m_output)) {
        wlr_output_state_set_mode(&state, mode);
      }
    }
    wlr_output_commit_state(m_output, &state);
    wlr_output_state_finish(&state);

    wlr_output_layout_output* layoutOutput = wlr_output_layout_add_auto(m_server->outputLayout(), m_output);
    m_sceneOutput = wlr_scene_output_create(m_server->scene(), m_output);
    wlr_scene_output_layout_add_output(m_server->sceneLayout(), layoutOutput, m_sceneOutput);

    for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      m_layerTrees[layer] = wlr_scene_tree_create(&m_server->scene()->tree);
    }
    m_popupTree = wlr_scene_tree_create(&m_server->scene()->tree);
    fixSceneOrder();

    arrangeLayers();
    m_workspaceGroup = std::make_unique<WorkspaceGroup>(*m_server, *this);
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

  void Output::fixSceneOrder() {
    wlr_scene_tree* xdg = m_server->xdgTree();
    // Bottom stack under windows, top/overlay above windows, popups above overlay.
    wlr_scene_node_place_below(&m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]->node, &xdg->node);
    wlr_scene_node_place_below(
        &m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]->node, &m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]->node
    );
    wlr_scene_node_place_above(&m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP]->node, &xdg->node);
    wlr_scene_node_place_above(
        &m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]->node, &m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_TOP]->node
    );
    wlr_scene_node_place_above(&m_popupTree->node, &m_layerTrees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]->node);
    m_server->raiseLockTree();
  }

  void Output::arrangeLayer(wlr_scene_tree* tree, const wlr_box* fullArea, wlr_box* usableArea, bool exclusive) {
    wlr_scene_node* node = nullptr;
    wl_list_for_each(node, &tree->children, link) {
      if (node->data == nullptr) {
        continue;
      }
      auto* sceneNode = static_cast<SceneNode*>(node->data);
      if (sceneNode->kind != SceneNodeKind::LayerSurface) {
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

    // Layer trees are output-local; pin them to the scene-output origin.
    for (uint32_t layer = 0; layer < kLayerCount; ++layer) {
      wlr_scene_node_set_position(&m_layerTrees[layer]->node, m_sceneOutput->x, m_sceneOutput->y);
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
  }

  void Output::onFrame(wl_listener* listener, void* /*data*/) {
    Output* self = wl_container_of(listener, self, m_frame);
    self->handleFrame();
  }

  void Output::onRequestState(wl_listener* listener, void* data) {
    Output* self = wl_container_of(listener, self, m_requestState);
    self->handleRequestState(data);
  }

  void Output::onDestroy(wl_listener* listener, void* /*data*/) {
    Output* self = wl_container_of(listener, self, m_destroy);
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
    }
    wlr_output_state_finish(&state);
    arrangeLayers();
    if (m_server->sessionLocked()) {
      m_server->updateLockBlank();
    }
    wlr_output_schedule_frame(m_output);
  }

  void Output::handleFrame() {
    if (m_hasDeferredMode) {
      m_hasDeferredMode = false;
      applyMode(m_deferredWidth, m_deferredHeight);
    }

    if (!wlr_scene_output_needs_frame(m_sceneOutput) || m_output->width <= 0 || m_output->height <= 0) {
      return;
    }

    m_inFrame = true;
    const bool ok = wlr_scene_output_commit(m_sceneOutput, nullptr);
    m_inFrame = false;

    if (m_hasDeferredMode) {
      m_hasDeferredMode = false;
      applyMode(m_deferredWidth, m_deferredHeight);
      return;
    }

    if (!ok) {
      wlr_output_schedule_frame(m_output);
      return;
    }

    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
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
      arrangeLayers();
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
    m_server->removeOutput(this);
  }

} // namespace umbriel
