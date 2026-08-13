#pragma once
#include "config/config.h"
#include "core/animation.h"
#include "layout/layout.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_ext_workspace_manager_v1;
struct wlr_scene_tree;

namespace umbriel {

  class DwindleLayout;
  class Output;
  class ScrollingLayout;
  class Server;
  class View;
  class WorkspaceGroup;

  class Workspace {
  public:
    Workspace(
        WorkspaceGroup& group, wlr_ext_workspace_handle_v1* handle, std::string name, size_t index,
        ResolvedLayoutConfig layoutConfig
    );
    ~Workspace();

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] wlr_ext_workspace_handle_v1* handle() const { return m_handle; }
    [[nodiscard]] WorkspaceGroup* group() const { return m_group; }
    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] size_t index() const { return m_index; }
    [[nodiscard]] bool active() const { return m_active; }
    [[nodiscard]] Layout& layout() { return *m_layout; }
    [[nodiscard]] const Layout& layout() const { return *m_layout; }
    // The one place a layout is downcast. Null unless this workspace is
    // scrolling, so callers that need scroll offsets, column positions, or row
    // weights ask for the layout that has them instead of asking every layout a
    // question only this one can answer.
    [[nodiscard]] ScrollingLayout* scrollingLayout();
    [[nodiscard]] const ScrollingLayout* scrollingLayout() const;
    [[nodiscard]] DwindleLayout* dwindleLayout();
    [[nodiscard]] const ResolvedLayoutConfig& layoutConfig() const { return m_layoutConfig; }
    [[nodiscard]] LayoutMode layoutMode() const { return m_layoutMode; }
    [[nodiscard]] View* focusedView() const { return m_focusedView; }
    [[nodiscard]] int slideOffsetY() const { return m_slideOffsetY; }
    [[nodiscard]] wlr_scene_tree* viewLayer(bool tiled) const { return tiled ? m_tiledLayer : m_floatingLayer; }
    [[nodiscard]] wlr_scene_tree* shadowLayer() const { return m_shadowLayer; }
    [[nodiscard]] wlr_scene_tree* fullscreenTree() const { return m_fullscreenTree; }
    [[nodiscard]] bool switchTransitionActive() const { return m_inSwitchTransition; }
    [[nodiscard]] bool isSwitchTransitionView(const View* view) const;

    void setActive(bool active);
    void setFocusedView(View* view);
    void syncFloatingStack(View* view);
    void restackFloatingViews();
    void addView(View* view, bool attachToLayout = true);
    View* removeView(View* view);
    void layoutAttach(View* view, std::optional<double> initialWidth = std::nullopt);
    void layoutDetach(View* view, bool animate = false);
    void arrange(bool animate = true);
    // Record that the layout is stale instead of rebuilding it now. The work
    // runs once, before the next frame, however many times this is called in
    // between: a touchpad swipe marks on every motion event, and unrelated
    // paths reached in the same frame (a focus change, a config reload, a
    // client's fullscreen commit) each used to arrange on their own.
    //
    // Prefer this to arrange(). Call arrange() directly only when the code
    // immediately afterwards reads the arranged geometry back out of the
    // layout -- targetBox() is the only thing arrange() produces that is not
    // simply applied to the scene, and a stale one would be read.
    void markArrange(bool animate = true);
    void flushArrange();
    void syncViewPresentation(View* view);
    [[nodiscard]] View* focusAdjacent(int direction) const;
    [[nodiscard]] View* focusVertical(int direction) const;
    bool moveFocusedColumn(int direction);
    bool consumeFocusedLeft();
    bool expelFocusedRight();
    bool moveFocusedVertical(int direction);
    bool cycleFocusedWidth();
    bool setFocusedWidth(double fraction);
    bool toggleFocusedFullWidth();
    bool toggleFocusedFullscreen();
    bool toggleFocusedFloating();
    void ensureFocusedVisible();
    [[nodiscard]] double scrollFractionToReveal(const View* view) const;
    void applyVisibility();
    void beginSwitchTransition();
    void showSwitchViews();
    void endSwitchTransition();
    void setSlideOffset(double y);
    void applyConfig(std::string name, size_t index, ResolvedLayoutConfig layoutConfig);
    void rename(std::string name, size_t index);

    [[nodiscard]] std::vector<View*> allViews() const { return m_views; }
    [[nodiscard]] bool hasViews() const { return !m_views.empty(); }

  private:
    void applyPositions(bool animate);
    // Pull the scroll offset back into [0, maxScroll]. Only for removals: a
    // touchpad swipe overscrolls on purpose.
    void clampScrollToRange();
    WorkspaceGroup* m_group = nullptr;
    wlr_ext_workspace_handle_v1* m_handle = nullptr;
    std::string m_name;
    size_t m_index = 0;
    bool m_active = false;
    std::vector<View*> m_views;
    std::vector<View*> m_floatingStack;
    std::unique_ptr<Layout> m_layout;
    ResolvedLayoutConfig m_layoutConfig;
    LayoutMode m_layoutMode = LayoutMode::Scrolling;
    View* m_focusedView = nullptr;
    bool m_inSwitchTransition = false;
    bool m_arrangePending = false;
    bool m_arrangeAnimate = true;
    int m_slideOffsetY = 0;
    std::vector<View*> m_switchViews;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_tree* m_shadowLayer = nullptr;
    wlr_scene_tree* m_tiledLayer = nullptr;
    wlr_scene_tree* m_floatingLayer = nullptr;
    wlr_scene_tree* m_fullscreenTree = nullptr;
  };

  class WorkspaceGroup : public Animatable {
  public:
    WorkspaceGroup(Server& server, Output& output);
    ~WorkspaceGroup();

    WorkspaceGroup(const WorkspaceGroup&) = delete;
    WorkspaceGroup& operator=(const WorkspaceGroup&) = delete;

    [[nodiscard]] Output* output() const { return m_output; }
    [[nodiscard]] Server* server() const { return m_server; }
    [[nodiscard]] wlr_ext_workspace_group_handle_v1* handle() const { return m_handle; }
    [[nodiscard]] Workspace* active() const { return m_active; }
    [[nodiscard]] Workspace* previous() const { return m_previous; }
    [[nodiscard]] bool dynamic() const { return m_dynamic; }
    [[nodiscard]] Workspace* workspaceAt(size_t index) const;
    [[nodiscard]] Workspace* workspaceAtClamped(size_t index) const;
    [[nodiscard]] Workspace* workspaceNamed(std::string_view name) const;
    [[nodiscard]] Workspace* workspaceForSelector(std::string_view name) const;
    [[nodiscard]] Workspace* workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const;
    [[nodiscard]] size_t workspaceCount() const { return m_workspaces.size(); }

    void activate(Workspace* workspace, bool animate = true);
    void select(Workspace* workspace);
    void deactivate(Workspace* workspace);
    Workspace* createWorkspace(const char* name);
    void reconcileConfig();
    void reconcileDynamic();
    // Every workspace, not just the active one: a client can change fullscreen
    // state while another workspace is showing, and that workspace still owes
    // it a configure at the right size.
    void flushArrange();

    [[nodiscard]] bool slideActive() const { return m_slide.base != nullptr; }
    bool slideBegin(bool includePrev, bool includeNext);
    void slideApply(double progress);
    void slideSettle(int delta);
    void slideFinish();
    // Advances the workspace slide; returns true while it is still running.
    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Workspaces; }
    bool tickAnimations(uint64_t nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override { return m_slideAnim.animating(); }
    [[nodiscard]] bool animatesOn(const Output* output) const override { return m_output == output; }

  private:
    std::unique_ptr<Workspace> createConfiguredWorkspace(ResolvedWorkspace workspace, size_t index);
    Workspace* appendDynamicWorkspace();

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
    bool m_dynamic = false;
    uint32_t m_nextHandleSerial = 1;
    std::vector<std::unique_ptr<Workspace>> m_workspaces;
    AnimatedValue m_slideAnim;
    Slide m_slide;
  };

} // namespace umbriel
