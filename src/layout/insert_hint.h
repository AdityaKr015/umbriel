#pragma once

#include "core/animation.h"

extern "C" {
#include <wlr/util/box.h>
}
struct wlr_scene_rect;
struct wlr_scene_tree;

namespace umbriel {

  class Server;
  class Output;
  class Workspace;

  class InsertHint {
  public:
    explicit InsertHint(Server& server);
    ~InsertHint();

    InsertHint(const InsertHint&) = delete;
    InsertHint& operator=(const InsertHint&) = delete;

    void show(Workspace* workspace, int gapIndex);
    void showRow(Workspace* workspace, int columnIndex, int rowIndex);
    void showBox(Workspace* workspace, const wlr_box& geometry);

    // World-coordinate hint rectangles for a scrolling workspace rendered at
    // horizontal offset `scroll`. Overview reuses these to draw the same hint
    // scaled into a workspace thumbnail. Zero-sized when not applicable.
    [[nodiscard]] static wlr_box gapHintBox(const Workspace& workspace, int gapIndex, double scroll);
    [[nodiscard]] static wlr_box rowHintBox(const Workspace& workspace, int columnIndex, int rowIndex, double scroll);

    void hide();
    [[nodiscard]] bool visible() const { return m_visible; }
    // Advances the fade; returns true while it is still running.
    bool tickAnimations(uint64_t nowMsec);
    [[nodiscard]] bool hasActiveAnimations() const { return m_fade.animating(); }
    [[nodiscard]] Output* output() const { return m_output; }

  private:
    void ensureScene();
    void scheduleFrame(Workspace* workspace) const;
    void setAlpha(float alpha);
    void showGeometry(Workspace* workspace, int x, int y, int width, int height);

    Server* m_server = nullptr;
    Output* m_output = nullptr;
    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_rect* m_rect = nullptr;
    AnimatedValue m_fade;
    bool m_visible = false;
  };

} // namespace umbriel
