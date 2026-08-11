#include "workspace/scratchpad.h"

#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace umbriel {

  ScratchpadManager::ScratchpadManager(Server& server, wlr_scene_tree* root, wlr_scene_tree* shadowRoot)
      : m_server(&server), m_root(root), m_shadowRoot(shadowRoot) {}

  bool ScratchpadManager::contains(const View* view) const {
    return std::ranges::any_of(m_entries, [view](const Entry& entry) { return entry.view == view; });
  }

  bool ScratchpadManager::moveFocusedToScratchpad(Output* output) {
    if (output == nullptr || m_server == nullptr || m_root == nullptr || m_shadowRoot == nullptr) {
      return false;
    }
    Workspace* workspace = output->workspaceGroup() != nullptr ? output->workspaceGroup()->active() : nullptr;
    View* view = workspace != nullptr ? workspace->focusedView() : nullptr;
    if (view == nullptr || !view->mapped() || contains(view)) {
      return false;
    }

    Entry entry{
        .view = view,
        .output = output,
        .returnOutput = {},
        .returnWorkspace = {},
        .returnTiled = view->tiled(),
    };
    if (Workspace* previous = view->workspace()) {
      entry.returnWorkspace = previous->name();
      if (previous->group() != nullptr && previous->group()->output() != nullptr) {
        entry.returnOutput = previous->group()->output()->wlr()->name;
      }
    }
    if (view->toplevel()->scheduled.fullscreen || view->toplevel()->current.fullscreen) {
      view->toggleFullscreen();
    }
    if (entry.returnTiled) {
      const int x = view->sceneTree()->node.x;
      const int y = view->sceneTree()->node.y;
      view->setFloating(true);
      view->cancelPositionAnimation();
      view->setPosition(x, y);
    }
    view->setWorkspace(nullptr);
    wlr_scene_node_reparent(&view->sceneTree()->node, m_root);
    view->reparentShadow(m_shadowRoot);
    view->setScratchpadBorder(true);
    m_entries.push_back(std::move(entry));
    setVisible(output, false);
    m_server->refocus(output);
    return true;
  }

  void ScratchpadManager::setVisible(Output* output, bool visible) {
    if (output == nullptr) {
      return;
    }
    if (visible) {
      if (std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
        m_visibleOutputs.push_back(output);
      }
    } else {
      std::erase(m_visibleOutputs, output);
    }
    for (const Entry& entry : m_entries) {
      if (entry.output == output && entry.view != nullptr) {
        entry.view->setOnActiveWorkspace(visible);
        entry.view->setNodeEnabled(visible);
      }
    }
  }

  bool ScratchpadManager::toggle(Output* output) {
    if (output == nullptr
        || std::ranges::none_of(m_entries, [output](const Entry& entry) { return entry.output == output; })) {
      return false;
    }
    const bool show = std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end();
    setVisible(output, show);
    if (show) {
      if (View* view = focused(output)) {
        m_server->focusView(view);
      }
    } else {
      m_server->refocus(output);
    }
    return true;
  }

  View* ScratchpadManager::focused(Output* output) const {
    if (std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
      return nullptr;
    }
    if (m_focusedView != nullptr) {
      const auto entry =
          std::ranges::find_if(m_entries, [this](const Entry& item) { return item.view == m_focusedView; });
      if (entry != m_entries.end() && entry->output == output) {
        return m_focusedView;
      }
    }
    for (const Entry& entry : m_entries) {
      if (entry.output == output) {
        return entry.view;
      }
    }
    return nullptr;
  }

  void ScratchpadManager::noteFocus(View* view) { m_focusedView = contains(view) ? view : nullptr; }

  bool ScratchpadManager::focusNext(Output* output) {
    if (output == nullptr || std::ranges::find(m_visibleOutputs, output) == m_visibleOutputs.end()) {
      return false;
    }
    std::vector<View*> views;
    for (const Entry& entry : m_entries) {
      if (entry.output == output && entry.view != nullptr && entry.view->mapped()) {
        views.push_back(entry.view);
      }
    }
    if (views.empty()) {
      return false;
    }
    View* current = focused(output);
    const auto it = std::ranges::find(views, current);
    View* target = it == views.end() || std::next(it) == views.end() ? views.front() : *std::next(it);
    m_server->focusView(target, FocusReason::Directional);
    return true;
  }

  bool ScratchpadManager::restoreFocused(Output* output) {
    View* view = focused(output);
    if (view == nullptr) {
      return false;
    }
    const auto it = std::ranges::find_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
    if (it == m_entries.end()) {
      return false;
    }
    Entry entry = std::move(*it);
    m_entries.erase(it);
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    Output* restoreOutput = m_server->outputFromName(entry.returnOutput);
    if (restoreOutput == nullptr) {
      restoreOutput = output;
    }
    Workspace* workspace = restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr
        ? restoreOutput->workspaceGroup()->workspaceNamed(entry.returnWorkspace)
        : nullptr;
    if (workspace == nullptr && restoreOutput != nullptr && restoreOutput->workspaceGroup() != nullptr) {
      workspace = restoreOutput->workspaceGroup()->active();
    }
    view->reparentShadow(nullptr);
    view->setScratchpadBorder(false);
    view->setWorkspace(workspace, false);
    if (entry.returnTiled) {
      view->setFloating(false);
    } else {
      view->setFloating(true);
    }
    if (workspace != nullptr) {
      workspace->syncViewPresentation(view);
    }
    m_server->focusView(view);
    return true;
  }

  void ScratchpadManager::remove(View* view) {
    if (view != nullptr) {
      view->setScratchpadBorder(false);
    }
    if (m_focusedView == view) {
      m_focusedView = nullptr;
    }
    std::erase_if(m_entries, [view](const Entry& entry) { return entry.view == view; });
  }

  void ScratchpadManager::moveOutput(Output* from, Output* to) {
    const bool wasVisible = std::ranges::find(m_visibleOutputs, from) != m_visibleOutputs.end();
    if (wasVisible) {
      setVisible(from, false);
    }
    for (Entry& entry : m_entries) {
      if (entry.output == from) {
        entry.output = to;
      }
    }
    if (wasVisible && to != nullptr) {
      setVisible(to, true);
    }
  }

} // namespace umbriel
