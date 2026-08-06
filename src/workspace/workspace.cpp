#include "workspace/workspace.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ranges>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {
    constexpr Logger kLog("workspace");

    constexpr uint32_t kWorkspaceCaps = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE
        | EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE;

    constexpr uint32_t kGroupCaps = EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE;
  } // namespace

  Workspace::Workspace(
      WorkspaceGroup& group, wlr_ext_workspace_handle_v1* handle, std::string name, size_t index,
      ResolvedLayoutConfig layoutConfig
  )
      : m_group(&group), m_handle(handle), m_name(std::move(name)), m_index(index),
        m_layout(createLayout(layoutConfig.mode)), m_layoutConfig(std::move(layoutConfig)),
        m_layoutMode(m_layoutConfig.mode) {
    m_layout->setConfig(&m_layoutConfig);
    m_handle->data = this;
    wlr_ext_workspace_handle_v1_set_group(m_handle, m_group->handle());
    wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
    const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
    wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
    m_tree = wlr_scene_tree_create(m_group->server()->xdgTree());
    m_shadowLayer = wlr_scene_tree_create(m_tree);
    wlr_scene_node_lower_to_bottom(&m_shadowLayer->node);
    m_fullscreenTree = wlr_scene_tree_create(m_group->server()->fullscreenTree());
  }

  Workspace::~Workspace() {
    if (m_scrollAnim != 0) {
      m_group->server()->animator().cancel(m_scrollAnim);
      m_scrollAnim = 0;
    }
    for (View* view : m_views) {
      view->cancelPositionAnimation();
      view->detachWorkspace();
    }
    m_views.clear();
    if (m_shadowLayer != nullptr) {
      wlr_scene_node_destroy(&m_shadowLayer->node);
      m_shadowLayer = nullptr;
    }
    if (m_tree != nullptr) {
      wlr_scene_tree* fallback = m_group->server()->xdgTree();
      wlr_scene_node* child;
      wlr_scene_node* tmp;
      wl_list_for_each_safe(child, tmp, &m_tree->children, link) { wlr_scene_node_reparent(child, fallback); }
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }
    if (m_fullscreenTree != nullptr) {
      wlr_scene_tree* fallback = m_group->server()->fullscreenTree();
      wlr_scene_node* child;
      wlr_scene_node* tmp;
      wl_list_for_each_safe(child, tmp, &m_fullscreenTree->children, link) { wlr_scene_node_reparent(child, fallback); }
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
    } else if (m_scrollAnim != 0) {
      m_group->server()->animator().cancel(m_scrollAnim);
      m_scrollAnim = 0;
    }
  }

  void Workspace::setFocusedView(View* view) {
    if (view == nullptr || view->workspace() == this) {
      m_focusedView = view;
    }
  }

  void Workspace::addView(View* view) {
    if (view == nullptr || std::ranges::find(m_views, view) != m_views.end()) {
      return;
    }
    m_views.push_back(view);
    const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
    wlr_scene_node_reparent(&view->sceneTree()->node, fs ? m_fullscreenTree : m_tree);
    view->reparentShadow(m_shadowLayer);
    applyVisibility();
    layoutAttach(view);
  }

  View* Workspace::removeView(View* view) {
    if (view == nullptr) {
      return nullptr;
    }
    const bool fs = view->toplevel()->current.fullscreen || view->toplevel()->scheduled.fullscreen;
    wlr_scene_node_reparent(
        &view->sceneTree()->node, fs ? m_group->server()->fullscreenTree() : m_group->server()->xdgTree()
    );
    view->reparentShadow(nullptr);
    const int removedColumn = m_layout->columnOf(view);
    m_layout->removeView(view);
    std::erase(m_views, view);
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
    return replacement;
  }

  void Workspace::layoutAttach(View* view) {
    if (view == nullptr || !view->mapped() || !view->tiled() || m_layout->columnOf(view) >= 0) {
      return;
    }
    const int focusedColumn = m_layout->columnOf(m_focusedView);
    const int index = focusedColumn >= 0 ? focusedColumn + 1 : static_cast<int>(m_layout->columns().size());
    m_layout->insertView(view, index);
    arrange();
  }

  void Workspace::layoutDetach(View* view) {
    m_layout->removeView(view);
    arrange(false);
  }

  void Workspace::arrange(bool animate) {
    if ((!m_active && !m_inSwitchTransition) || m_group == nullptr || m_group->output() == nullptr) {
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
      const wlr_box& geometry = view->toplevel()->base->geometry;
      const XdgSizeHints hints = xdgSizeHints(view->toplevel());
      const int width = clampXdgWidth(target.width, hints);
      const int height = clampXdgHeight(target.height, hints);
      if (geometry.width != width || geometry.height != height) {
        wlr_xdg_toplevel_set_size(view->toplevel(), width, height);
      }
    }

    if (m_layoutMode == LayoutMode::Scrolling) {
      const double targetScroll = m_layout->scroll();
      const bool scrollChanged = std::abs(targetScroll - m_visualScroll) > 0.01;
      if (m_scrollAnim != 0) {
        m_group->server()->animator().cancel(m_scrollAnim);
        m_scrollAnim = 0;
      }
      if (animate && scrollChanged) {
        for (View* view : m_views) {
          view->cancelPositionAnimation();
        }
        m_scrollAnim = m_group->server()->animator().animate(
            m_visualScroll, targetScroll, config().appearance.animationMs, Easing::EaseOutCubic,
            [this](double value) {
              m_visualScroll = value;
              applyPositions(false);
            },
            [this] { m_scrollAnim = 0; }
        );
        applyPositions(false);
        wlr_output_schedule_frame(output->wlr());
        return;
      }
      m_visualScroll = targetScroll;
    } else {
      if (m_scrollAnim != 0) {
        m_group->server()->animator().cancel(m_scrollAnim);
        m_scrollAnim = 0;
      }
      m_visualScroll = 0;
    }

    applyPositions(animate);
  }

  void Workspace::syncViewPresentation(View* view) {
    if ((!m_active && !m_inSwitchTransition)
        || view == nullptr
        || !view->mapped()
        || m_group == nullptr
        || m_group->output() == nullptr) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);

    if (view->toplevel()->scheduled.fullscreen) {
      const int col = m_layout->columnOf(view);
      if (col < 0) {
        if (m_active) {
          view->setNodeEnabled(true);
          view->applyFullscreenLayout();
        } else {
          view->setNodeEnabled(false);
        }
        return;
      }
      if (m_layoutMode == LayoutMode::Scrolling) {
        const int viewportWidth = std::max(1, output->usableArea().width - 2 * m_layoutConfig.edgePad);
        wlr_box target = outputBox;
        target.x = outputBox.x + m_layout->columnX(col, viewportWidth) - static_cast<int>(std::lround(m_visualScroll));
        wlr_box clipTarget = target;
        clipTarget.y += m_slideOffsetY;
        wlr_box decorated = clipTarget;
        wlr_box intersection{};
        const bool visible = wlr_box_intersection(&intersection, &decorated, &outputBox);
        view->setNodeEnabled(visible);
        view->setOutputClip(visible ? &intersection : nullptr, clipTarget, outputBox);
      } else {
        wlr_box target = m_layout->targetBox(view);
        wlr_box clipTarget = target;
        clipTarget.y += m_slideOffsetY;
        wlr_box intersection{};
        const bool visible = wlr_box_intersection(&intersection, &clipTarget, &outputBox);
        view->setNodeEnabled(visible);
        view->setOutputClip(visible ? &intersection : nullptr, clipTarget, outputBox);
      }
      return;
    }

    if (m_layout->columnOf(view) < 0) {
      const wlr_box& geometry = view->toplevel()->base->geometry;
      wlr_box target{
          .x = view->sceneTree()->node.x,
          .y = view->sceneTree()->node.y + m_slideOffsetY,
          .width = geometry.width,
          .height = geometry.height,
      };
      wlr_box intersection{};
      const bool visible = wlr_box_intersection(&intersection, &target, &outputBox);
      view->setNodeEnabled(visible);
      view->setOutputClip(visible ? &intersection : nullptr, target, outputBox);
      return;
    }

    const int scrollOffset = (m_layoutMode == LayoutMode::Scrolling)
        ? static_cast<int>(std::lround(m_layout->scroll() - m_visualScroll))
        : 0;
    wlr_box target = m_layout->targetBox(view);
    target.x += scrollOffset;
    const int border = config().appearance.totalBorderWidth();
    const int presentW = view->presentedWidth(target);
    const int presentH = view->presentedHeight(target);
    wlr_box clipTarget = target;
    clipTarget.y += m_slideOffsetY;
    wlr_box decorated{clipTarget.x - border, clipTarget.y - border, presentW + 2 * border, presentH + 2 * border};
    wlr_box intersection{};
    const bool visible = wlr_box_intersection(&intersection, &decorated, &outputBox);
    view->setNodeEnabled(visible);
    view->setOutputClip(visible ? &intersection : nullptr, clipTarget, outputBox);
  }

  void Workspace::applyPositions(bool animate) {
    if ((!m_active && !m_inSwitchTransition) || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);
    const int scrollOffset = (m_layoutMode == LayoutMode::Scrolling)
        ? static_cast<int>(std::lround(m_layout->scroll() - m_visualScroll))
        : 0;
    const int border = config().appearance.totalBorderWidth();
    const int viewportWidth = std::max(1, output->usableArea().width - 2 * m_layoutConfig.edgePad);

    for (View* view : m_views) {
      if (view == nullptr || !view->mapped()) {
        continue;
      }
      if (view->toplevel()->scheduled.fullscreen) {
        const int col = m_layout->columnOf(view);
        if (col < 0) {
          if (m_active) {
            view->setNodeEnabled(true);
            view->applyFullscreenLayout();
          }
          continue;
        }
        if (m_layoutMode == LayoutMode::Scrolling) {
          wlr_box target = outputBox;
          target.x =
              outputBox.x + m_layout->columnX(col, viewportWidth) - static_cast<int>(std::lround(m_visualScroll));
          if (animate) {
            view->animateTo(target.x, target.y);
          } else {
            view->setPosition(target.x, target.y);
          }
          wlr_scene_subsurface_tree_set_clip(&view->sceneTree()->node, nullptr);
          wlr_box clipTarget = target;
          clipTarget.y += m_slideOffsetY;
          wlr_box decorated = clipTarget;
          wlr_box intersection{};
          const bool visible = wlr_box_intersection(&intersection, &decorated, &outputBox);
          view->setNodeEnabled(visible);
          view->setOutputClip(visible ? &intersection : nullptr, clipTarget, outputBox);
        } else {
          wlr_box target = m_layout->targetBox(view);
          if (animate) {
            view->animateTo(target.x, target.y);
          } else {
            view->setPosition(target.x, target.y);
          }
          wlr_scene_subsurface_tree_set_clip(&view->sceneTree()->node, nullptr);
          wlr_box clipTarget = target;
          clipTarget.y += m_slideOffsetY;
          wlr_box intersection{};
          const bool visible = wlr_box_intersection(&intersection, &clipTarget, &outputBox);
          view->setNodeEnabled(visible);
          view->setOutputClip(visible ? &intersection : nullptr, clipTarget, outputBox);
        }
        continue;
      }
      if (m_layout->columnOf(view) < 0) {
        continue;
      }
      wlr_box target = m_layout->targetBox(view);
      target.x += scrollOffset;
      if (animate) {
        view->animateTo(target.x, target.y);
      } else {
        view->setPosition(target.x, target.y);
      }
      const int presentW = view->presentedWidth(target);
      const int presentH = view->presentedHeight(target);
      wlr_box clipTarget = target;
      clipTarget.y += m_slideOffsetY;
      wlr_box decorated{clipTarget.x - border, clipTarget.y - border, presentW + 2 * border, presentH + 2 * border};
      wlr_box intersection{};
      const bool visible = wlr_box_intersection(&intersection, &decorated, &outputBox);
      view->setNodeEnabled(visible);
      view->setOutputClip(visible ? &intersection : nullptr, clipTarget, outputBox);
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
      view->setOnActiveWorkspace(m_active);
    }
  }

  bool Workspace::isSwitchTransitionView(const View* view) const {
    return std::ranges::find(m_switchViews, view) != m_switchViews.end();
  }

  void Workspace::beginSwitchTransition() {
    m_inSwitchTransition = true;
    m_switchViews.clear();
    for (View* view : m_views) {
      if (view->mapped() && (view->sceneTree()->node.enabled || !m_active)) {
        view->cancelFadeAnimation();
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
      if (view->mapped()) {
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
  }

  void Workspace::recreateLayout() {
    // Re-resolve config from the current global config for this workspace's output and index.
    const char* outputName = m_group->output()->wlr()->name;
    auto resolved = resolveWorkspacesForOutput(outputName);
    if (m_index < resolved.size()) {
      m_layoutConfig = std::move(resolved[m_index].layout);
    } else {
      m_layoutConfig = resolveGlobalLayout();
    }
    if (m_layout != nullptr && m_layout->mode() == m_layoutConfig.mode) {
      // Mode unchanged — just re-arrange with potentially new gaps/widths.
      m_layout->setConfig(&m_layoutConfig);
      arrange();
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
    m_visualScroll = 0;
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
    const size_t count = resolved.empty() ? kDefaultCount : resolved.size();
    for (size_t i = 0; i < count; ++i) {
      char id[64];
      std::snprintf(id, sizeof(id), "%s:%zu", outputName, i + 1);
      const std::string wsName = i < resolved.size() ? std::move(resolved[i].name) : std::to_string(i + 1);
      ResolvedLayoutConfig wsLayout = i < resolved.size() ? std::move(resolved[i].layout) : resolveGlobalLayout();
      wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
      m_workspaces.push_back(std::make_unique<Workspace>(*this, handle, wsName, i, std::move(wsLayout)));
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

  Workspace* WorkspaceGroup::workspaceAt(size_t index) const {
    if (index >= m_workspaces.size()) {
      return nullptr;
    }
    return m_workspaces[index].get();
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
    if (m_switchAnim != 0) {
      m_server->animator().cancel(m_switchAnim);
      m_switchAnim = 0;
    }
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
    m_switchAnim = m_server->animator().animate(
        m_slide.progress, static_cast<double>(delta), config().appearance.animationMs, Easing::EaseOutCubic,
        [this](double v) { slideApply(v); }, [this] { slideFinish(); }
    );
  }

  void WorkspaceGroup::activate(Workspace* workspace, bool animate) {
    if (workspace == nullptr || workspace->group() != this) {
      return;
    }
    if (m_active == workspace) {
      return;
    }
    slideFinish();
    const bool doAnimate = animate && m_active != nullptr && !m_server->sessionLocked();
    if (!doAnimate) {
      if (m_active != nullptr) {
        m_previous = m_active;
        m_active->setActive(false);
      }
      m_active = workspace;
      m_active->setActive(true);
      kLog.debug("activate workspace {} on {}", m_active->name(), m_output->wlr()->name);
      return;
    }
    wlr_box box{};
    wlr_output_layout_get_box(m_server->outputLayout(), m_output->wlr(), &box);
    if (box.height <= 0) {
      m_previous = m_active;
      m_active->setActive(false);
      m_active = workspace;
      m_active->setActive(true);
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

  void WorkspaceGroup::activateIndex(size_t index) {
    Workspace* workspace = workspaceAt(index);
    if (workspace == nullptr) {
      return;
    }
    if (m_active == workspace) {
      if (!config().general.workspaceBackAndForth || m_previous == nullptr || m_previous == m_active) {
        return;
      }
      activate(m_previous);
    } else {
      activate(workspace);
    }
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
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    const size_t index = m_workspaces.size();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    char id[64];
    std::snprintf(id, sizeof(id), "%s:%zu", outputName, index + 1);
    std::string wsName = (name != nullptr && name[0] != '\0') ? name : std::to_string(index + 1);
    wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
    m_workspaces.push_back(std::make_unique<Workspace>(*this, handle, std::move(wsName), index, resolveGlobalLayout()));
    return m_workspaces.back().get();
  }

} // namespace umbriel
