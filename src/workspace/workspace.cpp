#include "workspace/workspace.h"

#include "core/log.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <cstdio>

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
    for (View* view : m_views) {
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
  }

  void Workspace::addView(View* view) {
    if (view == nullptr) {
      return;
    }
    if (std::find(m_views.begin(), m_views.end(), view) != m_views.end()) {
      return;
    }
    m_views.push_back(view);
    applyVisibility();
  }

  void Workspace::removeView(View* view) { std::erase(m_views, view); }

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
