#pragma once
#include "core/animation.h"
#include "layout/scrolling.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_ext_workspace_manager_v1;
struct wlr_scene_tree;

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
    [[nodiscard]] int slideOffsetY() const { return m_slideOffsetY; }
    [[nodiscard]] wlr_scene_tree* tree() const { return m_tree; }
    [[nodiscard]] wlr_scene_tree* fullscreenTree() const { return m_fullscreenTree; }
    [[nodiscard]] bool switchTransitionActive() const { return m_inSwitchTransition; }
    [[nodiscard]] bool isSwitchTransitionView(const View* view) const;

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
    bool toggleFocusedFullscreen();
    bool toggleFocusedFloating();
    void ensureFocusedVisible();
    // Fraction of viewport width that revealing `view` would scroll (0.0 = already visible).
    [[nodiscard]] double scrollFractionToReveal(const View* view) const;
    void applyVisibility();
    void beginSwitchTransition();
    void showSwitchViews();
    void endSwitchTransition();
    void setSlideOffset(double y);

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
    bool m_inSwitchTransition = false;
    int m_slideOffsetY = 0;
    std::vector<View*> m_switchViews;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_tree* m_fullscreenTree = nullptr;
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
    [[nodiscard]] Workspace* previous() const { return m_previous; }
    [[nodiscard]] Workspace* workspaceAt(size_t index) const;
    [[nodiscard]] Workspace* workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const;

    void activate(Workspace* workspace, bool animate = true);
    void activateIndex(size_t index);
    void deactivate(Workspace* workspace);
    Workspace* createWorkspace(const char* name);

    [[nodiscard]] bool slideActive() const { return m_slide.base != nullptr; }
    bool slideBegin(bool includePrev, bool includeNext);
    void slideApply(double progress);
    void slideSettle(int delta);
    void slideFinish();

  private:
    struct Slide {
      Workspace* base = nullptr;
      Workspace* up = nullptr;
      Workspace* down = nullptr;
      double height = 0;
      double progress = 0;
    };

    Server* m_server = nullptr;
    Output* m_output = nullptr;
    wlr_ext_workspace_group_handle_v1* m_handle = nullptr;
    Workspace* m_active = nullptr;
    Workspace* m_previous = nullptr;
    std::vector<std::unique_ptr<Workspace>> m_workspaces;
    AnimId m_switchAnim = 0;
    Slide m_slide;
  };

} // namespace umbriel
