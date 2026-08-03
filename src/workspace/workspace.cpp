#include "workspace/workspace.h"

#include "config/config.h"
#include "core/log.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
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
  } // namespace

  Workspace::Workspace(WorkspaceGroup& group, wlr_ext_workspace_handle_v1* handle, std::string name, size_t index)
      : m_group(&group), m_handle(handle), m_name(std::move(name)), m_index(index) {
    m_handle->data = this;
    wlr_ext_workspace_handle_v1_set_group(m_handle, m_group->handle());
    wlr_ext_workspace_handle_v1_set_name(m_handle, m_name.c_str());
    const uint32_t coords[1] = {static_cast<uint32_t>(m_index)};
    wlr_ext_workspace_handle_v1_set_coordinates(m_handle, coords, 1);
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
    if (view == nullptr || std::find(m_views.begin(), m_views.end(), view) != m_views.end()) {
      return;
    }
    m_views.push_back(view);
    applyVisibility();
    layoutAttach(view);
  }

  View* Workspace::removeView(View* view) {
    if (view == nullptr) {
      return nullptr;
    }
    const int removedColumn = m_layout.columnOf(view);
    m_layout.removeView(view);
    std::erase(m_views, view);

    View* replacement = nullptr;
    if (m_focusedView == view) {
      if (!m_layout.columns().empty() && removedColumn >= 0) {
        const int index = std::min(removedColumn, static_cast<int>(m_layout.columns().size()) - 1);
        const Column& column = m_layout.columns()[static_cast<size_t>(index)];
        if (!column.views.empty()) {
          replacement = column.views.front();
        }
      }
      m_focusedView = replacement;
    }
    arrange();
    return replacement;
  }

  void Workspace::layoutAttach(View* view) {
    if (view == nullptr || !view->mapped() || !view->tiled() || m_layout.columnOf(view) >= 0) {
      return;
    }
    const int focusedColumn = m_layout.columnOf(m_focusedView);
    const int index = focusedColumn >= 0 ? focusedColumn + 1 : static_cast<int>(m_layout.columns().size());
    m_layout.insertView(view, index);
    arrange();
  }

  void Workspace::layoutDetach(View* view) {
    m_layout.removeView(view);
    // Instant reflow so remaining tiles do not animate under the grabbed window.
    arrange(false);
  }

  void Workspace::arrange(bool animate) {
    if (!m_active || m_group == nullptr || m_group->output() == nullptr) {
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

    m_layout.arrange(usable);
    for (const Column& column : m_layout.columns()) {
      for (View* view : column.views) {
        if (view == nullptr || !view->mapped()) {
          continue;
        }
        const wlr_box target = m_layout.targetBox(view);
        const wlr_box& geometry = view->toplevel()->base->geometry;
        if (geometry.width != target.width || geometry.height != target.height) {
          wlr_xdg_toplevel_set_size(view->toplevel(), target.width, target.height);
        }
      }
    }

    const double targetScroll = m_layout.scroll();
    const bool scrollChanged = std::abs(targetScroll - m_visualScroll) > 0.01;
    if (m_scrollAnim != 0) {
      m_group->server()->animator().cancel(m_scrollAnim);
      m_scrollAnim = 0;
    }
    if (animate && scrollChanged) {
      for (const Column& column : m_layout.columns()) {
        for (View* view : column.views) {
          view->cancelPositionAnimation();
        }
      }
      m_scrollAnim = m_group->server()->animator().animate(
          m_visualScroll, targetScroll, config().appearance.animationMs,
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
    applyPositions(animate);
  }

  void Workspace::syncViewPresentation(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    if (m_layout.columnOf(view) < 0) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);
    const int scrollOffset = static_cast<int>(std::lround(m_layout.scroll() - m_visualScroll));
    wlr_box target = m_layout.targetBox(view);
    target.x += scrollOffset;
    const int border = config().appearance.borderWidth;
    wlr_box decorated = target;
    decorated.x -= border;
    decorated.y -= border;
    decorated.width += 2 * border;
    decorated.height += 2 * border;
    wlr_box intersection{};
    const bool visible = wlr_box_intersection(&intersection, &decorated, &outputBox);
    wlr_scene_node_set_enabled(&view->sceneTree()->node, visible);
    view->setOutputClip(visible ? &intersection : nullptr, target, outputBox);
  }

  void Workspace::applyPositions(bool animate) {
    if (!m_active || m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    Output* output = m_group->output();
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_group->server()->outputLayout(), output->wlr(), &outputBox);
    const int scrollOffset = static_cast<int>(std::lround(m_layout.scroll() - m_visualScroll));
    const int border = config().appearance.borderWidth;
    for (const Column& column : m_layout.columns()) {
      for (View* view : column.views) {
        if (view == nullptr || !view->mapped()) {
          continue;
        }
        wlr_box target = m_layout.targetBox(view);
        target.x += scrollOffset;
        if (animate) {
          view->animateTo(target.x, target.y);
        } else {
          view->setPosition(target.x, target.y);
        }
        // Include borders so near-edge windows stay clipped to this output.
        wlr_box decorated = target;
        decorated.x -= border;
        decorated.y -= border;
        decorated.width += 2 * border;
        decorated.height += 2 * border;
        wlr_box intersection{};
        const bool visible = wlr_box_intersection(&intersection, &decorated, &outputBox);
        wlr_scene_node_set_enabled(&view->sceneTree()->node, visible);
        view->setOutputClip(visible ? &intersection : nullptr, target, outputBox);
      }
    }
  }

  View* Workspace::focusAdjacent(int direction) const {
    const int current = m_layout.columnOf(m_focusedView);
    const int target = current + direction;
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout.columns().size())) {
      return nullptr;
    }
    const Column& column = m_layout.columns()[static_cast<size_t>(target)];
    return column.views.empty() ? nullptr : column.views.front();
  }

  View* Workspace::focusVertical(int direction) const {
    const int column = m_layout.columnOf(m_focusedView);
    const int row = m_layout.rowOf(m_focusedView);
    if (column < 0 || row < 0) {
      return nullptr;
    }
    const auto& views = m_layout.columns()[static_cast<size_t>(column)].views;
    const int target = row + direction;
    return target < 0 || target >= static_cast<int>(views.size()) ? nullptr : views[static_cast<size_t>(target)];
  }

  bool Workspace::moveFocusedColumn(int direction) {
    const int current = m_layout.columnOf(m_focusedView);
    const int target = current + direction;
    if (current < 0 || target < 0 || target >= static_cast<int>(m_layout.columns().size())) {
      return false;
    }
    m_layout.moveColumn(current, target);
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::consumeFocusedLeft() {
    if (!m_layout.consumeLeft(m_focusedView)) {
      return false;
    }
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::expelFocusedRight() {
    if (!m_layout.expelRight(m_focusedView)) {
      return false;
    }
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::moveFocusedVertical(int direction) {
    if (!m_layout.moveViewVertical(m_focusedView, direction)) {
      return false;
    }
    arrange();
    return true;
  }

  bool Workspace::cycleFocusedWidth() {
    const int column = m_layout.columnOf(m_focusedView);
    if (!m_layout.cycleWidth(column)) {
      return false;
    }
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), false);
    ensureFocusedVisible();
    arrange();
    return true;
  }

  bool Workspace::toggleFocusedFullWidth() {
    const int column = m_layout.columnOf(m_focusedView);
    if (column < 0) {
      return false;
    }
    const bool fullWidth = m_layout.toggleFullWidth(column);
    wlr_xdg_toplevel_set_maximized(m_focusedView->toplevel(), fullWidth);
    ensureFocusedVisible();
    // Snap layout immediately; client size catches up on configure ack.
    arrange(false);
    return true;
  }

  void Workspace::ensureFocusedVisible() {
    if (m_group == nullptr || m_group->output() == nullptr) {
      return;
    }
    const int column = m_layout.columnOf(m_focusedView);
    const int viewportWidth = std::max(1, m_group->output()->usableArea().width - 2 * config().layout.gap);
    m_layout.ensureVisible(column, viewportWidth);
  }

  void Workspace::applyVisibility() {
    for (View* view : m_views) {
      view->setOnActiveWorkspace(m_active);
    }
  }

  WorkspaceGroup::WorkspaceGroup(Server& server, Output& output) : m_server(&server), m_output(&output) {
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    m_handle = wlr_ext_workspace_group_handle_v1_create(manager, kGroupCaps);
    m_handle->data = this;
    wlr_ext_workspace_group_handle_v1_output_enter(m_handle, m_output->wlr());

    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    for (size_t i = 0; i < kDefaultCount; ++i) {
      char id[64];
      char name[16];
      std::snprintf(id, sizeof(id), "%s:%zu", outputName, i + 1);
      std::snprintf(name, sizeof(name), "%zu", i + 1);
      wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
      m_workspaces.push_back(std::make_unique<Workspace>(*this, handle, name, i));
    }

    activate(m_workspaces.front().get());
    kLog.info("workspace group for {} with {} workspaces", outputName, kDefaultCount);
  }

  WorkspaceGroup::~WorkspaceGroup() {
    m_active = nullptr;
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

  void WorkspaceGroup::activate(Workspace* workspace) {
    if (workspace == nullptr || workspace->group() != this) {
      return;
    }
    if (m_active == workspace) {
      return;
    }
    if (m_active != nullptr) {
      m_active->setActive(false);
    }
    m_active = workspace;
    m_active->setActive(true);
    kLog.debug("activate workspace {} on {}", m_active->name(), m_output->wlr()->name);
  }

  void WorkspaceGroup::activateIndex(size_t index) {
    if (Workspace* workspace = workspaceAt(index)) {
      activate(workspace);
      m_server->refocus();
    }
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
      activate(fallback);
      m_server->refocus();
      return;
    }
    m_active->setActive(false);
    m_active = nullptr;
    m_server->refocus();
  }

  Workspace* WorkspaceGroup::createWorkspace(const char* name) {
    wlr_ext_workspace_manager_v1* manager = m_server->workspaceManager();
    const size_t index = m_workspaces.size();
    const char* outputName = m_output->wlr()->name != nullptr ? m_output->wlr()->name : "output";
    char id[64];
    std::snprintf(id, sizeof(id), "%s:%zu", outputName, index + 1);
    std::string wsName = (name != nullptr && name[0] != '\0') ? name : std::to_string(index + 1);
    wlr_ext_workspace_handle_v1* handle = wlr_ext_workspace_handle_v1_create(manager, id, kWorkspaceCaps);
    m_workspaces.push_back(std::make_unique<Workspace>(*this, handle, std::move(wsName), index));
    return m_workspaces.back().get();
  }

} // namespace umbriel
