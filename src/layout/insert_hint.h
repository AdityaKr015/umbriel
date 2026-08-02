#pragma once

#include "core/animation.h"

struct wlr_scene_rect;
struct wlr_scene_shadow;
struct wlr_scene_tree;

namespace umbriel {

  class Server;
  class Workspace;

  inline constexpr int kHintWidth = 20;

  class InsertHint {
  public:
    explicit InsertHint(Server& server);
    ~InsertHint();

    InsertHint(const InsertHint&) = delete;
    InsertHint& operator=(const InsertHint&) = delete;

    void show(Workspace* workspace, int gapIndex);
    void showRow(Workspace* workspace, int columnIndex, int rowIndex);
    void hide();
    [[nodiscard]] bool visible() const { return m_visible; }

  private:
    void ensureScene();
    void scheduleFrame(Workspace* workspace) const;
    void setAlpha(float alpha);
    void showGeometry(Workspace* workspace, int x, int y, int width, int height);

    Server* m_server = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_shadow* m_shadow = nullptr;
    wlr_scene_rect* m_rect = nullptr;
    AnimId m_positionAnim = 0;
    AnimId m_fadeAnim = 0;
    bool m_visible = false;
  };

} // namespace umbriel
