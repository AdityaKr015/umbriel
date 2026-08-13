#include "workspace/workspace.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "output/output.h"
#include "overview/overview.h"
#include "server/server.h"
#include "view/view.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {
    constexpr Logger kLog("workspace");

    constexpr uint32_t kWorkspaceCaps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE
        | EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE;

    constexpr uint32_t kGroupCaps = EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE;

    // The bridge between the layout's opaque View identity and the client state
    // it needs to size that view. Workspace owns both sides, so it owns the
    // lookup; layout/ stays free of view/ and its geometry stays testable.
    LayoutConstraints viewLayoutConstraints(const View* view) {
      const wlr_xdg_toplevel* toplevel = view != nullptr ? view->toplevel() : nullptr;
      const XdgSizeHints hints = xdgSizeHints(toplevel);
      return {
          .minWidth = hints.minWidth,
          .minHeight = hints.minHeight,
          .maxWidth = hints.maxWidth,
          .maxHeight = hints.maxHeight,
          .fullscreen = toplevel != nullptr && toplevel->scheduled.fullscreen,
      };
    }
  } // namespace

  Workspace::Workspace(
      WorkspaceGroup& group, wlr_ext_workspace_handle_v1* handle, std::string name, size_t index,
      ResolvedLayoutConfig layoutConfig
  )
      : m_group(&group), m_handle(handle), m_name(std::move(name)), m_index(index),
        m_layout(createLayout(layoutConfig.mode)), m_layoutConfig(std::move(layoutConfig)),
        m_layoutMode(m_layoutConfig.mode) {
    m_layout->setConfig(&m_layoutConfig);
    m_layout->setConstraints(&viewLayoutConstraints);
    m_handle->data = this;
    wlr_ext_workspace_handle_v1_set_group(m_handle, m_group->handle());
    wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
    const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
    wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
    m_tree = wlr_scene_tree_create(m_group->server()->xdgTree());
    // Focus raises only within a layer: floating views can never fall below tiles.
    m_shadowLayer = wlr_scene_tree_create(m_tree);
    m_tiledLayer = wlr_scene_tree_create(m_tree);
    m_floatingLayer = wlr_scene_tree_create(m_tree);
    m_fullscreenTree = wlr_scene_tree_create(m_group->server()->fullscreenTree());
  }

  Workspace::~Workspace() {
    for (View* view : m_views) {
      view->cancelPositionAnimation();
      const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
      wlr_scene_node_reparent(
          &view->sceneTree()->node, fs ? m_group->server()->fullscreenTree() : m_group->server()->xdgTree()
      );
      view->detachWorkspace();
    }
    m_views.clear();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
      m_shadowLayer = nullptr;
      m_tiledLayer = nullptr;
      m_floatingLayer = nullptr;
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_node_destroy(&m_fullscreenTree->node);
      m_fullscreenTree = nullptr;
    }
    if (m_handle != nullptr) {
      if (m_handle->data == this) {
        m_handle->data = nullptr;
      }
      wlr_ext_workspace_handle_v1_destroy(m_handle);
      m_handle = nullptr;
    }
  }

  void Workspace::setActive(bool active) {
    if (m_active == active) {
      return;
    }
    m_active = active;
    wlr_ext_workspace_handle_v1_set_active(m_handle, active);
    applyVisibility();
    if (active) {
      arrange(false);
    }
  }

  void Workspace::setFocusedView(View* view) {
    if (view == nullptr || view->workspace() == this) {
      m_focusedView = view;
      if (view != nullptr && view->floating()) {
        std::erase(m_floatingStack, view);
        m_floatingStack.push_back(view);
        restackFloatingViews();
      }
    }
  }

  void Workspace::syncFloatingStack(View* view) {
    if (view == nullptr || view->workspace() != this) {
      return;
    }
    if (view->floating()) {
      if (std::ranges::find(m_floatingStack, view) == m_floatingStack.end()) {
        m_floatingStack.push_back(view);
      }
    } else {
      std::erase(m_floatingStack, view);
    }
    restackFloatingViews();
  }

  void Workspace::restackFloatingViews() {
    for (View* view : m_floatingStack) {
      if (view != nullptr && view->workspace() == this && view->floating()) {
        view->raiseToTop();
      }
    }
  }

  void Workspace::addView(View* view, bool attachToLayout) {
    if (view == nullptr || std::ranges::find(m_views, view) != m_views.end()) {
      return;
    }
    m_views.push_back(view);
    const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
    if (!view->pinned()) {
      wlr_scene_node_reparent(&view->sceneTree()->node, fs ? m_fullscreenTree : viewLayer(view->tiled()));
      view->reparentShadow(m_shadowLayer);
    }
    syncFloatingStack(view);
    applyVisibility();
    if (attachToLayout) {
      layoutAttach(view);
    }
    m_group->reconcileDynamic();
  }

  View* Workspace::removeView(View* view) {
    if (view == nullptr) {
      return nullptr;
    }
    if (!view->pinned()) {
      const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
      wlr_scene_node_reparent(
          &view->sceneTree()->node, fs ? m_group->server()->fullscreenTree() : m_group->server()->xdgTree()
      );
      view->reparentShadow(nullptr);
    }
    const int removedColumn = m_layout->columnOf(view);
    m_layout->removeView(view);
    std::erase(m_views, view);
    std::erase(m_floatingStack, view);
    std::erase(m_switchViews, view);

    View* replacement = nullptr;
    if (m_focusedView == view) {
      if (removedColumn >= 0) {
        for (View* v : m_views) {
          if (v->tiled() && m_layout->columnOf(v) >= 0) {
            replacement = v;
            break;
          }
        }
      }
      m_focusedView = replacement;
    }
    arrange();
    m_group->reconcileDynamic();
    return replacement;
  }

  void Workspace::layoutAttach(View* view, std::optional<double> initialWidth) {
    if (view == nullptr || !view->mapped() || !view->tiled() || m_layout->columnOf(view) >= 0) {
      return;
    }
    const int focusedColumn = m_layout->columnOf(m_focusedView);
    const int index = focusedColumn >= 0 ? focusedColumn + 1 : static_cast<int>(m_layout->columns().size());
    m_layout->insertView(view, index);
    // default_width is a viewport fraction: scrolling only. Dwindle ignores it.
    if (initialWidth && m_layoutMode == LayoutMode::Scrolling) {
      m_layout->setWidthFraction(m_layout->columnOf(view), *initialWidth);
    }
    arrange(true);
  }

  void Workspace::layoutDetach(View* view, bool animate) {
    m_layout->removeView(view);
    arrange(animate);
  }

  void Workspace::arrange(bool animate) {
    // Layout math and client configures must run even for hidden workspaces:
    // clients (games especially) change fullscreen state while another
    // workspace is active, and skipping the configure here leaves them with a
    // stale size (fullscreen at tile size, windowed at output size, ...).
    if (m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    Output* output = m_group->output();
    wlr_box usable = output->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &usable);
    }
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }

    m_layout->arrange(usable);
    for (View* view : m_views) {
      if (view == nullptr || !view->mapped() || !view->tiled()) {
        continue;
      }
      if (m_layout->columnOf(view) < 0) {
        continue;
      }
      if (view->toplevel()->scheduled.fullscreen) {
        wlr_box fullArea{};
        wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &fullArea);
        if (fullArea.width > 0
            && fullArea.height > 0
            && (view->toplevel()->scheduled.width != fullArea.width
                || view->toplevel()->scheduled.height != fullArea.height)) {
          wlr_xdg_toplevel_set_size(view->toplevel(), fullArea.width, fullArea.height);
        }
        continue;
      }
      const wlr_box target = m_layout->targetBox(view);
      const XdgSizeHints hints = xdgSizeHints(view->toplevel());
      const int width = clampXdgWidth(target.width, hints);
      const int height = clampXdgHeight(target.height, hints);
      const auto& scheduled = view->toplevel()->scheduled;
      if (scheduled.width != width || scheduled.height != height) {
        wlr_xdg_toplevel_set_size(view->toplevel(), width, height);
        // Start the presentation animation when the compositor changes the
        // assigned size. Client geometry can differ from a stable configure,
        // notably with Chromium CSD, and must not replay the resize on focus.
        if (animate) {
          view->beginResizeAnimation(width, height);
        }
      }
    }

    // Card geometry derives from the layout math above, so it stays correct for
    // hidden workspaces too (resizes, drag gaps, config-driven relayout).
    if (Overview* overview = m_group->server()->overview(); overview != nullptr && overview->active()) {
      overview->onWorkspaceArranged(this);
    }

    // Visual state below (scroll, positions) only applies while visible.
    if (!m_active && !m_inSwitchTransition) {
      return;
    }

    // One positioning path for both layouts: the layout owns the scroll, the
    // targets below already include it, and each view animates or snaps itself.
    applyPositions(animate);
  }

  void Workspace::syncViewPresentation(View* view) {
    if (view == nullptr || !view->mapped() || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    // A window under an interactive move spans outputs unclipped; leave it as
    // clipGrabbedViewToOutput set it (re-clipping mid-drag flickers A<->B).
    if (Cursor* cursor = m_group->server()->cursor(); cursor != nullptr && cursor->isDraggingView(view)) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);

    // Clip against the node's CURRENT position, not the layout target: during
    // position animations (column swaps, drag drops) the node lags the target,
    // and clips computed at the target land displaced on screen (cut-off
    // borders that reappear as the window settles).
    const wlr_scene_node& node = view->sceneTree()->node;
    const int border = config().appearance.totalBorderWidth();
    const auto grow = [border](const wlr_box& box, int width, int height) {
      return wlr_box{box.x - border, box.y - border, width + 2 * border, height + 2 * border};
    };

    // Each case only decides two boxes: the region the view occupies, and the
    // region its decorations occupy. Everything after is common.
    wlr_box target{};
    wlr_box decorated{};

    if (view->pinned()) {
      // Pinned views sit outside the workspace, so no slide offset applies, and
      // they are sized from committed geometry rather than the presented size.
      const wlr_box& geometry = view->toplevel()->base->geometry;
      target = {node.x, node.y, geometry.width, geometry.height};
      decorated = grow(target, target.width, target.height);
    } else {
      if (!m_active && !m_inSwitchTransition) {
        return;
      }

      if (view->toplevel()->scheduled.fullscreen) {
        if (m_layout->columnOf(view) < 0) {
          if (m_active) {
            view->setNodeEnabled(true);
            view->applyFullscreenLayout();
          } else {
            view->setNodeEnabled(false);
          }
          return;
        }
        // Fullscreen covers the output and draws no decorations.
        target = {node.x, node.y + m_slideOffsetY, outputBox.width, outputBox.height};
        decorated = target;
      } else {
        // Floating views follow committed geometry; tiled ones follow the box
        // the layout assigned them.
        const wlr_box sized =
            m_layout->columnOf(view) < 0 ? view->toplevel()->base->geometry : m_layout->targetBox(view);
        target = {node.x, node.y + m_slideOffsetY, sized.width, sized.height};
        decorated = grow(target, view->presentedWidth(target), view->presentedHeight(target));
      }
    }

    wlr_box intersection{};
    const bool visible = wlr_box_intersection(&intersection, &decorated, &outputBox);
    view->setNodeEnabled(visible);
    view->setOutputClip(visible ? &intersection : nullptr, target, outputBox);
  }

  void Workspace::applyPositions(bool animate) {
    if ((!m_active && !m_inSwitchTransition) || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);
    const int viewportWidth = std::max(1, output->usableArea().width - 2 * m_layoutConfig.edgePad);

    // Position first, then let syncViewPresentation derive enable + clip from
    // the node's current position so animated and resting views share one path.
    for (View* view : m_views) {
      if (view == nullptr || !view->mapped()) {
        continue;
      }
      if (view->toplevel()->scheduled.fullscreen) {
        const int col = m_layout->columnOf(view);
        if (col >= 0) {
          wlr_box target = outputBox; // fullscreen fills the whole output, not the dwindle tile box
          if (m_layoutMode == LayoutMode::Scrolling) {
            target.x =
                outputBox.x + m_layout->columnX(col, viewportWidth) - static_cast<int>(std::lround(m_layout->scroll()));
          }
          if (animate) {
            view->animateTo(target.x, target.y);
          } else {
            view->setPosition(target.x, target.y);
          }
        }
        syncViewPresentation(view);
        continue;
      }
      if (m_layout->columnOf(view) < 0) {
        // Floating (non-fullscreen): clip + enable against the home output.
        view->clampFloatingPosition();
        syncViewPresentation(view);
        continue;
      }
      wlr_box target = m_layout->targetBox(view);
      if (animate) {
        view->animateTo(target.x, target.y);
      } else {
        view->setPosition(target.x, target.y);
      }
      syncViewPresentation(view);
    }
  }

  View* Workspace::focusAdjacent(int direction) const {
    const int current = m_layout->columnOf(m_focusedView);
    const int target = current + direction;
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout->columns().size())) {
      return nullptr;
    }
    const Column& column = m_layout->columns()[static_cast<size_t>(target)];
    return column.views.empty() ? nullptr : column.views.front();
  }

  View* Workspace::focusVertical(int direction) const {
    if (View* vertical = m_layout->focusVerticalLeaf(m_focusedView, direction)) {
      return vertical;
    }
    const int column = m_layout->columnOf(m_focusedView);
    const int row = m_layout->rowOf(m_focusedView);
    if (column < 0 || row < 0) {
      return nullptr;
    }
    const auto& views = m_layout->columns()[static_cast<size_t>(column)].views;
    const int target = row + direction;
    return target < 0 || target >= static_cast<int>(views.size()) ? nullptr : views[static_cast<size_t>(target)];
  }

  bool Workspace::moveFocusedColumn(int direction) {
    const int current = m_layout->columnOf(m_focusedView);
    const int target = current + direction;
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout->columns().size())) {
      return false;
    }
    m_layout->moveColumn(current, target);
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::consumeFocusedLeft() {
    if (!m_layout->consumeLeft(m_focusedView)) {
      return false;
    }
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::expelFocusedRight() {
    if (!m_layout->expelRight(m_focusedView)) {
      return false;
    }
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::moveFocusedVertical(int direction) {
    if (!m_layout->moveViewVertical(m_focusedView, direction)) {
      return false;
    }
    arrange();
    return true;
  }

  bool Workspace::cycleFocusedWidth() {
    const int column = m_layout->columnOf(m_focusedView);
    if (!m_layout->cycleWidth(column)) {
      return false;
    }
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), false);
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::setFocusedWidth(double fraction) {
    const int column = m_layout->columnOf(m_focusedView);
    if (!m_layout->setWidthFraction(column, fraction)) {
      return false;
    }
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), false);
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::toggleFocusedFullWidth() {
    const int column = m_layout->columnOf(m_focusedView);
    if (column < 0) {
      return false;
    }
    const bool fullWidth = m_layout->toggleFullWidth(column);
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), fullWidth);
    ensureFocusedVisible();
    arrange(false);
    return true;
  }

  bool Workspace::toggleFocusedFullscreen() {
    if (m_focusedView == nullptr || !m_focusedView->mapped()) {
      return false;
    }
    m_focusedView->toggleFullscreen();
    return true;
  }

  bool Workspace::toggleFocusedFloating() {
    if (m_focusedView == nullptr || !m_focusedView->mapped()) {
      return false;
    }
    m_focusedView->toggleFloating();
    return true;
  }

  void Workspace::ensureFocusedVisible() {
    if (m_group == nullptr || m_group->output() == nullptr || m_layoutMode != LayoutMode::Scrolling) {
      return;
    }
    const int column = m_layout->columnOf(m_focusedView);
    const int viewportWidth = std::max(1, m_group->output()->usableArea().width - 2 * m_layoutConfig.edgePad);
    m_layout->ensureVisible(column, viewportWidth);
  }

  double Workspace::scrollFractionToReveal(const View* view) const {
    if (m_group == nullptr || m_group->output() == nullptr || m_layoutMode != LayoutMode::Scrolling) {
      return 0.0;
    }
    const int column = m_layout->columnOf(view);
    const int viewportWidth = std::max(1, m_group->output()->usableArea().width - 2 * m_layoutConfig.edgePad);
    return m_layout->scrollAmountToEnsureVisible(column, viewportWidth);
  }

  void Workspace::applyVisibility() {
    for (View* view : m_views) {
      if (view->pinned()) {
        continue;
      }
      view->setOnActiveWorkspace(m_active);
      // Persistent resting state: an inactive workspace keeps its nodes disabled
      // so the shared scene never renders them on any output. Active (and
      // in-transition) views are enabled + clipped to their home output by
      // syncViewPresentation (arrange / slide), which replaces the old
      // per-render-pass enable/disable.
      if (!m_active && !m_inSwitchTransition) {
        view->setNodeEnabled(false);
      }
    }
  }

  bool Workspace::isSwitchTransitionView(const View* view) const {
    return std::ranges::find(m_switchViews, view) != m_switchViews.end();
  }

  void Workspace::beginSwitchTransition() {
    m_inSwitchTransition = true;
    m_switchViews.clear();
    for (View* view : m_views) {
      if (!view->pinned() && view->mapped() && (view->sceneTree()->node.enabled || !m_active)) {
        m_switchViews.push_back(view);
      }
    }
  }

  void Workspace::showSwitchViews() {
    for (View* view : m_switchViews) {
      if (!m_active) {
        view->setNodeEnabled(true);
      }
    }
  }

  void Workspace::setSlideOffset(double y) {
    m_slideOffsetY = static_cast<int>(std::lround(y));
    if (m_tree != nullptr) {
      wlr_scene_node_set_position(&m_tree->node, 0, m_slideOffsetY);
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_node_set_position(&m_fullscreenTree->node, 0, m_slideOffsetY);
    }
    for (View* view : m_views) {
      if (!view->pinned() && view->mapped()) {
        syncViewPresentation(view);
      }
    }
  }

  void Workspace::endSwitchTransition() {
    m_inSwitchTransition = false;
    for (View* view : m_switchViews) {
      view->setFadeAlpha(1.0F);
      if (!m_active) {
        view->setNodeEnabled(false);
      }
    }
    m_switchViews.clear();
    // setSlideOffset() refreshes visibility and clips, but it does not move tiled
    // scene nodes to their authoritative horizontal strip positions. Reconcile
    // after an interrupted switch so a fullscreen column cannot remain off-screen.
    if (m_active) {
      arrange(false);
    }
  }

  void Workspace::rename(std::string name, size_t index) {
    if (m_name != name) {
      m_name = std::move(name);
      wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
    }
    if (m_index != index) {
      m_index = index;
      const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
      wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
    }
  }

  void Workspace::applyConfig(std::string name, size_t index, ResolvedLayoutConfig layoutConfig) {
    rename(std::move(name), index);
    m_layoutConfig = std::move(layoutConfig);
    if (m_layout != nullptr && m_layout->mode() == m_layoutConfig.mode) {
      m_layout->setConfig(&m_layoutConfig);
      m_layout->setConstraints(&viewLayoutConstraints);
      arrange(true);
      return;
    }
    std::vector<View*> tiledViews;
    for (View* view : m_views) {
      if (m_layout != nullptr && m_layout->columnOf(view) >= 0) {
        m_layout->removeView(view);
        tiledViews.push_back(view);
      }
    }
    m_layoutMode = m_layoutConfig.mode;
    m_layout = createLayout(m_layoutMode);
    m_layout->setConfig(&m_layoutConfig);
    m_layout->setConstraints(&viewLayoutConstraints);
    for (View* view : tiledViews) {
      m_layout->insertView(view, static_cast<int>(m_layout->columns().size()));
    }
    arrange();
  }

  WorkspaceGroup::WorkspaceGroup(Server& server, Output& output) : m_server(&server), m_output(&output) {
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    m_handle = wlr_ext_workspace_group_handle_v1_create(manager, kGroupCaps);
    m_handle->data = this;
    wlr_ext_workspace_group_handle_v1_output_enter(m_handle, m_output->wlr());

    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    auto resolved = resolveWorkspacesForOutput(outputName);
    m_dynamic = resolved.dynamic;
    const size_t count = resolved.workspaces.size();
    m_workspaces.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      m_workspaces.push_back(createConfiguredWorkspace(std::move(resolved.workspaces[i]), i));
    }

    activate(m_workspaces.front().get());
    kLog.info("workspace group for {} with {} workspaces", outputName, count);
  }

  WorkspaceGroup::~WorkspaceGroup() {
    slideFinish();
    m_active = nullptr;
    m_previous = nullptr;
    if (m_handle != nullptr && m_output != nullptr && m_output->wlr() != nullptr) {
      wlr_ext_workspace_group_handle_v1_output_leave(m_handle, m_output->wlr());
    }
    m_workspaces.clear();
    if (m_handle != nullptr) {
      if (m_handle->data == this) {
        m_handle->data = nullptr;
      }
      wlr_ext_workspace_group_handle_v1_destroy(m_handle);
      m_handle = nullptr;
    }
  }
  std::unique_ptr<Workspace> WorkspaceGroup::createConfiguredWorkspace(ResolvedWorkspace workspace, size_t index) {
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    char id[64];
    std::snprintf(id, sizeof(id), "%s:%u", outputName, m_nextHandleSerial++);
    wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
    return std::make_unique<Workspace>(*this, handle, std::move(workspace.name), index, std::move(workspace.layout));
  }

  Workspace* WorkspaceGroup::appendDynamicWorkspace() {
    const size_t index = m_workspaces.size();
    std::string name = std::to_string(index + 1);
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    ResolvedLayoutConfig layout = resolveWorkspaceLayout(outputName, name, index);
    auto workspace = createConfiguredWorkspace({std::move(name), std::move(layout)}, index);
    Workspace* result = workspace.get();
    m_workspaces.push_back(std::move(workspace));
    return result;
  }

  void WorkspaceGroup::reconcileConfig() {
    slideFinish();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    auto resolvedSet = resolveWorkspacesForOutput(outputName);
    m_dynamic = resolvedSet.dynamic;

    if (m_dynamic) {
      auto old = std::move(m_workspaces);
      m_workspaces.clear();
      m_workspaces.reserve(old.size() + 1);
      for (auto& workspace : old) {
        if (workspace != nullptr && (workspace->hasViews() || workspace.get() == m_active)) {
          m_workspaces.push_back(std::move(workspace));
        } else if (workspace.get() == m_previous) {
          m_previous = nullptr;
        }
      }
      old.clear();

      if (m_workspaces.empty() || m_workspaces.back()->hasViews()) {
        appendDynamicWorkspace();
      }
      for (size_t index = 0; index < m_workspaces.size(); ++index) {
        const std::string name = std::to_string(index + 1);
        m_workspaces[index]->applyConfig(name, index, resolveWorkspaceLayout(outputName, name, index));
      }
      if (m_previous == m_active) {
        m_previous = nullptr;
      }
      kLog.info("reconciled {} to {} workspaces (0 windows relocated)", outputName, m_workspaces.size());
      return;
    }

    auto& resolved = resolvedSet.workspaces;
    auto old = std::move(m_workspaces);
    std::vector<std::unique_ptr<Workspace>> next(resolved.size());

    // Preserve workspace identity by name before using position as a fallback.
    for (size_t i = 0; i < resolved.size(); ++i) {
      const auto match = std::ranges::find_if(old, [&](const auto& workspace) {
        return workspace != nullptr && workspace->name() == resolved[i].name;
      });
      if (match != old.end()) {
        next[i] = std::move(*match);
      }
    }
    for (size_t i = 0; i < resolved.size(); ++i) {
      if (next[i] == nullptr && i < old.size() && old[i] != nullptr) {
        next[i] = std::move(old[i]);
      }
      if (next[i] != nullptr) {
        next[i]->applyConfig(std::move(resolved[i].name), i, std::move(resolved[i].layout));
      } else {
        next[i] = createConfiguredWorkspace(std::move(resolved[i]), i);
      }
    }

    const auto survives = [&](const Workspace* workspace) {
      return workspace != nullptr
          && std::ranges::any_of(next, [&](const auto& candidate) { return candidate.get() == workspace; });
    };
    const bool activeSurvives = survives(m_active);
    const bool previousSurvives = survives(m_previous);
    Workspace* replacementActive = activeSurvives ? m_active : nullptr;
    size_t relocatedViews = 0;

    m_workspaces = std::move(next);
    for (const auto& removed : old) {
      if (removed == nullptr) {
        continue;
      }
      Workspace* fallback = m_workspaces[std::min(removed->index(), m_workspaces.size() - 1)].get();
      if (removed.get() == m_active) {
        removed->setActive(false);
        replacementActive = fallback;
      }
      for (View* view : removed->allViews()) {
        view->setWorkspace(fallback);
        ++relocatedViews;
      }
    }

    if (!previousSurvives) {
      m_previous = nullptr;
    }
    if (!activeSurvives) {
      m_active = replacementActive;
      m_active->setActive(true);
    }
    if (m_previous == m_active) {
      m_previous = nullptr;
    }
    old.clear();

    if (relocatedViews > 0 || !activeSurvives) {
      m_server->cursor()->clearConstraint();
      m_server->refocus(m_output);
    }
    kLog.info("reconciled {} to {} workspaces ({} windows relocated)", outputName, m_workspaces.size(), relocatedViews);
  }

  void WorkspaceGroup::reconcileDynamic() {
    if (!m_dynamic || slideActive()) {
      return;
    }
    Overview* overview = m_server->overview();
    if (overview != nullptr && overview->active()) {
      return;
    }

    if (m_workspaces.size() > 1) {
      for (size_t index = m_workspaces.size() - 1; index-- > 0;) {
        Workspace* workspace = m_workspaces[index].get();
        if (!workspace->hasViews() && workspace != m_active) {
          if (m_previous == workspace) {
            m_previous = nullptr;
          }
          m_workspaces.erase(m_workspaces.begin() + static_cast<std::ptrdiff_t>(index));
        }
      }
    }
    if (m_workspaces.empty() || m_workspaces.back()->hasViews()) {
      appendDynamicWorkspace();
    }

    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    for (size_t index = 0; index < m_workspaces.size(); ++index) {
      const std::string name = std::to_string(index + 1);
      ResolvedLayoutConfig layout = resolveWorkspaceLayout(outputName, name, index);
      Workspace* workspace = m_workspaces[index].get();
      if (workspace->layoutConfig() != layout) {
        workspace->applyConfig(name, index, std::move(layout));
      } else if (workspace->name() != name || workspace->index() != index) {
        workspace->rename(name, index);
      }
    }
    if (m_previous == m_active) {
      m_previous = nullptr;
    }
  }

  Workspace* WorkspaceGroup::workspaceAt(size_t index) const {
    if (index >= m_workspaces.size()) {
      return nullptr;
    }
    return m_workspaces[index].get();
  }

  Workspace* WorkspaceGroup::workspaceAtClamped(size_t index) const {
    Workspace* workspace = workspaceAt(index);
    if (workspace == nullptr && m_dynamic && !m_workspaces.empty()) {
      return m_workspaces.back().get();
    }
    return workspace;
  }

  Workspace* WorkspaceGroup::workspaceNamed(std::string_view name) const {
    for (const auto& entry : m_workspaces) {
      if (entry->name() == name) {
        return entry.get();
      }
    }
    return nullptr;
  }

  Workspace* WorkspaceGroup::workspaceForSelector(std::string_view name) const {
    if (Workspace* workspace = workspaceNamed(name)) {
      return workspace;
    }
    if (!m_dynamic || name.empty() || !std::ranges::all_of(name, [](char value) {
          return value >= '0' && value <= '9';
        })) {
      return nullptr;
    }
    size_t index = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), index);
    if (error != std::errc{} || end != name.data() + name.size() || index < 1 || m_workspaces.empty()) {
      return nullptr;
    }
    return m_workspaces.back().get();
  }

  Workspace* WorkspaceGroup::workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const {
    for (const auto& entry : m_workspaces) {
      if (entry->handle() == handle) {
        return entry.get();
      }
    }
    return nullptr;
  }

  void WorkspaceGroup::slideFinish() {
    m_slideAnim.snap(0.0);
    if (m_slide.base != nullptr) {
      m_slide.base->endSwitchTransition();
      m_slide.base->setSlideOffset(0);
    }
    if (m_slide.up != nullptr) {
      m_slide.up->endSwitchTransition();
      m_slide.up->setSlideOffset(0);
    }
    if (m_slide.down != nullptr) {
      m_slide.down->endSwitchTransition();
      m_slide.down->setSlideOffset(0);
    }
    if (m_active != nullptr && m_active->switchTransitionActive()) {
      m_active->endSwitchTransition();
    }
    m_slide = {};
  }

  bool WorkspaceGroup::slideBegin(bool includePrev, bool includeNext) {
    if (m_active == nullptr) {
      return false;
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), m_output->wlr(), &box);
    if (box.height <= 0) {
      return false;
    }
    slideFinish();
    m_slide.base = m_active;
    m_slide.height = box.height;
    m_slide.progress = 0;
    const size_t idx = m_active->index();
    m_slide.up = (includePrev && idx > 0) ? workspaceAt(idx - 1) : nullptr;
    m_slide.down = includeNext ? workspaceAt(idx + 1) : nullptr;
    m_slide.base->beginSwitchTransition();
    if (m_slide.up != nullptr) {
      m_slide.up->beginSwitchTransition();
      m_slide.up->showSwitchViews();
      m_slide.up->arrange(false);
    }
    if (m_slide.down != nullptr) {
      m_slide.down->beginSwitchTransition();
      m_slide.down->showSwitchViews();
      m_slide.down->arrange(false);
    }
    slideApply(0.0);
    return true;
  }

  void WorkspaceGroup::slideApply(double progress) {
    m_slide.progress = progress;
    const double h = m_slide.height;
    m_slide.base->setSlideOffset(-progress * h);
    if (m_slide.down != nullptr) {
      m_slide.down->setSlideOffset((1.0 - progress) * h);
    }
    if (m_slide.up != nullptr) {
      m_slide.up->setSlideOffset((-1.0 - progress) * h);
    }
    wlr_output_schedule_frame(m_output->wlr());
  }

  void WorkspaceGroup::slideSettle(int delta) {
    Workspace* target = nullptr;
    if (delta < 0) {
      target = m_slide.up;
    } else if (delta > 0) {
      target = m_slide.down;
    }
    if (target == nullptr) {
      delta = 0;
      target = m_slide.base;
    }
    if (target != m_active) {
      m_previous = m_active;
      m_active->setActive(false);
      m_active = target;
      m_active->setActive(true);
      if (m_previous != nullptr) {
        m_previous->showSwitchViews();
      }
      Workspace* unused = (delta > 0) ? m_slide.up : m_slide.down;
      if (unused != nullptr) {
        unused->endSwitchTransition();
        unused->setSlideOffset(0);
      }
    }
    kLog.debug("slide workspace {} → {} on {}", m_slide.base->name(), target->name(), m_output->wlr()->name);
    m_slideAnim.snap(m_slide.progress);
    m_slideAnim.retarget(static_cast<double>(delta), config().appearance.animationMs, Easing::EaseOutCubic);
    wlr_output_schedule_frame(m_output->wlr());
  }

  bool WorkspaceGroup::tickAnimations(uint64_t nowMsec) {
    if (!m_slideAnim.tick(nowMsec)) {
      return false;
    }
    slideApply(m_slideAnim.current());
    if (!m_slideAnim.animating()) {
      slideFinish();
      reconcileDynamic();
      return false;
    }
    return true;
  }

  void WorkspaceGroup::activate(Workspace* workspace, bool animate) {
    if (workspace == nullptr || workspace->group() != this) {
      return;
    }
    if (m_active == workspace) {
      return;
    }
    slideFinish();
    Overview* overview = m_server->overview();
    const bool overviewActive = overview != nullptr && overview->active();
    // The real trees are hidden while overview runs: the filmstrip scroll is
    // the transition, so never start a slide underneath it.
    const bool doAnimate = animate && m_active != nullptr && !m_server->sessionLocked() && !overviewActive;
    if (!doAnimate) {
      if (m_active != nullptr) {
        m_previous = m_active;
        m_active->setActive(false);
      }
      m_active = workspace;
      m_active->setActive(true);
      kLog.debug("activate workspace {} on {}", m_active->name(), m_output->wlr()->name);
      if (overviewActive) {
        overview->onWorkspaceActivated(this);
      }
      reconcileDynamic();
      return;
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), m_output->wlr(), &box);
    if (box.height <= 0) {
      m_previous = m_active;
      m_active->setActive(false);
      m_active = workspace;
      m_active->setActive(true);
      reconcileDynamic();
      return;
    }
    const int sign = workspace->index() > m_active->index() ? 1 : -1;
    m_slide.base = m_active;
    m_slide.height = box.height;
    m_slide.progress = 0;
    if (sign > 0) {
      m_slide.down = workspace;
    } else {
      m_slide.up = workspace;
    }
    m_slide.base->beginSwitchTransition();
    workspace->beginSwitchTransition();
    slideApply(0.0);
    slideSettle(sign);
  }

  void WorkspaceGroup::select(Workspace* workspace) {
    if (workspace == nullptr || workspace->group() != this) {
      return;
    }
    Workspace* selected = workspace;
    if (m_active == workspace && config().workspaces.backAndForth && m_previous != nullptr && m_previous != m_active) {
      selected = m_previous;
    }
    activate(selected);
    m_server->cursor()->clearConstraint();
    m_server->refocus(m_output);
  }

  void WorkspaceGroup::deactivate(Workspace* workspace) {
    if (workspace == nullptr || m_active != workspace) {
      return;
    }
    Workspace* fallback = workspaceAt(0);
    if (fallback == workspace) {
      fallback = workspaceAt(1);
    }
    if (fallback != nullptr) {
      activate(fallback, false);
      m_server->cursor()->clearConstraint();
      m_server->refocus(m_output);
      return;
    }
    m_active->setActive(false);
    m_active = nullptr;
    m_server->cursor()->clearConstraint();
    m_server->refocus(m_output);
  }

  Workspace* WorkspaceGroup::createWorkspace(const char* name) {
    if (m_dynamic) {
      kLog.debug("using trailing dynamic workspace for create request on {}", m_output->wlr()->name);
      return m_workspaces.empty() ? appendDynamicWorkspace() : m_workspaces.back().get();
    }

    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    const size_t index = m_workspaces.size();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    char id[64];
    std::snprintf(id, sizeof(id), "%s:%u", outputName, m_nextHandleSerial++);
    std::string wsName = (name != nullptr && name[0] != '\0') ? name : std::to_string(index + 1);
    wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
    m_workspaces.push_back(std::make_unique<Workspace>(*this, handle, std::move(wsName), index, resolveGlobalLayout()));
    return m_workspaces.back().get();
  }

} // namespace umbriel
