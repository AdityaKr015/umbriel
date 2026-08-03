#pragma once
#include "core/animation.h"
#include "layout/scrolling.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_ext_workspace_manager_v1;

namespace umbriel {

  class Output;
  class Server;
  class View;
  class WorkspaceGroup;

  class Workspace {
  public:
    Workspace(WorkspaceGroup& group, wlr_ext_workspace_handle_v1* handle, std::string name, size_t index);
    ~Workspace();

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] wlr_ext_workspace_handle_v1* handle() const { return m_handle; }
    [[nodiscard]] WorkspaceGroup* group() const { return m_group; }
    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] size_t index() const { return m_index; }
    [[nodiscard]] bool active() const { return m_active; }
    [[nodiscard]] ScrollingLayout& layout() { return m_layout; }
    [[nodiscard]] const ScrollingLayout& layout() const { return m_layout; }
    [[nodiscard]] View* focusedView() const { return m_focusedView; }
    [[nodiscard]] double visualScroll() const { return m_visualScroll; }

    void setActive(bool active);
    void setFocusedView(View* view);
    void addView(View* view);
    View* removeView(View* view);
    void layoutAttach(View* view);
    void layoutDetach(View* view);
    void arrange(bool animate = true);
    void syncViewPresentation(View* view);
    [[nodiscard]] View* focusAdjacent(int direction) const;
    [[nodiscard]] View* focusVertical(int direction) const;
    bool moveFocusedColumn(int direction);
    bool consumeFocusedLeft();
    bool expelFocusedRight();
    bool moveFocusedVertical(int direction);
    bool cycleFocusedWidth();
    bool toggleFocusedFullWidth();
    void ensureFocusedVisible();
    void applyVisibility();

  private:
    void applyPositions(bool animate);
    WorkspaceGroup* m_group = nullptr;
    wlr_ext_workspace_handle_v1* m_handle = nullptr;
    std::string m_name;
    size_t m_index = 0;
    bool m_active = false;
    std::vector<View*> m_views;
    ScrollingLayout m_layout;
    View* m_focusedView = nullptr;
    double m_visualScroll = 0;
    AnimId m_scrollAnim = 0;
  };

  class WorkspaceGroup {
  public:
    static constexpr size_t kDefaultCount = 9;

    WorkspaceGroup(Server& server, Output& output);
    ~WorkspaceGroup();

    WorkspaceGroup(const WorkspaceGroup&) = delete;
    WorkspaceGroup& operator=(const WorkspaceGroup&) = delete;

    [[nodiscard]] Output* output() const { return m_output; }
    [[nodiscard]] Server* server() const { return m_server; }
    [[nodiscard]] wlr_ext_workspace_group_handle_v1* handle() const { return m_handle; }
    [[nodiscard]] Workspace* active() const { return m_active; }
    [[nodiscard]] Workspace* workspaceAt(size_t index) const;
    [[nodiscard]] Workspace* workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const;

    void activate(Workspace* workspace);
    void activateIndex(size_t index);
    void deactivate(Workspace* workspace);
    Workspace* createWorkspace(const char* name);

  private:
    Server* m_server = nullptr;
    Output* m_output = nullptr;
    wlr_ext_workspace_group_handle_v1* m_handle = nullptr;
    Workspace* m_active = nullptr;
    std::vector<std::unique_ptr<Workspace>> m_workspaces;
  };

} // namespace umbriel
