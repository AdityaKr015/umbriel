#include "layout/insert_hint.h"

#include "layout/scrolling.h"
#include "output/output.h"
#include "server/server.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr float kHintRed = 0.48F;
    constexpr float kHintGreen = 0.64F;
    constexpr float kHintBlue = 1.0F;
    constexpr float kHintAlpha = 0.45F;
    constexpr float kShadowAlpha = 0.35F;
  } // namespace

  InsertHint::InsertHint(Server& server) : m_server(&server) {}

  InsertHint::~InsertHint() {
    hide();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
      m_shadow = nullptr;
      m_rect = nullptr;
    }
  }

  void InsertHint::ensureScene() {
    if (m_tree != nullptr) {
      return;
    }
    m_tree = wlr_scene_tree_create(m_server->xdgTree());
    const float shadowColor[4] = {kHintRed, kHintGreen, kHintBlue, kShadowAlpha};
    m_shadow = wlr_scene_shadow_create(m_tree, kHintWidth, 1, kHintWidth / 2, 18.0F, shadowColor);
    const float rectColor[4] = {kHintRed, kHintGreen, kHintBlue, 0.0F};
    m_rect = wlr_scene_rect_create(m_tree, kHintWidth, 1, rectColor);
    wlr_scene_rect_set_corner_radius(m_rect, kHintWidth / 2);
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  void InsertHint::setAlpha(float alpha) {
    const float color[4] = {kHintRed, kHintGreen, kHintBlue, alpha};
    wlr_scene_rect_set_color(m_rect, color);
  }

  void InsertHint::scheduleFrame(Workspace* workspace) const {
    if (workspace != nullptr && workspace->group() != nullptr && workspace->group()->output() != nullptr) {
      wlr_output_schedule_frame(workspace->group()->output()->wlr());
    }
  }

  void InsertHint::show(Workspace* workspace, int gapIndex) {
    if (workspace == nullptr || workspace->group() == nullptr || workspace->group()->output() == nullptr) {
      return;
    }
    Output* output = workspace->group()->output();
    const wlr_box usable = output->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }
    const int viewportWidth = std::max(1, usable.width - 2 * kGap);
    const int columnCount = static_cast<int>(workspace->layout().columns().size());
    const int gap = std::clamp(gapIndex, 0, columnCount);
    int boundary = 0;
    if (gap == columnCount && gap > 0) {
      boundary = workspace->layout().columnX(gap, viewportWidth) - kGap;
    } else if (gap > 0) {
      boundary = workspace->layout().columnX(gap, viewportWidth) - kGap / 2;
    }
    const int targetX =
        usable.x + kGap + boundary - static_cast<int>(std::lround(workspace->visualScroll())) - kHintWidth / 2;
    showGeometry(workspace, targetX, usable.y + kGap, kHintWidth, std::max(1, usable.height - 2 * kGap));
  }

  void InsertHint::showRow(Workspace* workspace, int columnIndex, int rowIndex) {
    if (workspace == nullptr
        || workspace->group() == nullptr
        || workspace->group()->output() == nullptr
        || columnIndex < 0
        || columnIndex >= static_cast<int>(workspace->layout().columns().size())) {
      return;
    }
    Output* output = workspace->group()->output();
    const wlr_box usable = output->usableArea();
    const int viewportWidth = std::max(1, usable.width - 2 * kGap);
    const Column& column = workspace->layout().columns()[static_cast<size_t>(columnIndex)];
    const int row = std::clamp(rowIndex, 0, static_cast<int>(column.views.size()));
    int boundaryY = usable.y + kGap;
    if (row == static_cast<int>(column.views.size())) {
      boundaryY = usable.y + usable.height - kGap;
    } else if (row > 0) {
      boundaryY = workspace->layout().targetBox(column.views[static_cast<size_t>(row)]).y - kGap / 2;
    }
    const int targetX = usable.x
        + kGap
        + workspace->layout().columnX(columnIndex, viewportWidth)
        - static_cast<int>(std::lround(workspace->visualScroll()));
    const int width = workspace->layout().columnWidth(columnIndex, viewportWidth);
    showGeometry(workspace, targetX, boundaryY - kHintWidth / 2, width, kHintWidth);
  }

  void InsertHint::showGeometry(Workspace* workspace, int x, int y, int width, int height) {
    ensureScene();
    wlr_scene_rect_set_size(m_rect, width, height);
    wlr_scene_shadow_set_size(m_shadow, width, height);
    const int radius = std::min(width, height) / 2;
    wlr_scene_rect_set_corner_radius(m_rect, radius);
    wlr_scene_shadow_set_corner_radius(m_shadow, radius);

    if (m_positionAnim != 0) {
      m_server->animator().cancel(m_positionAnim);
      m_positionAnim = 0;
    }
    if (!m_visible) {
      wlr_scene_node_set_position(&m_tree->node, x, y);
      wlr_scene_node_set_enabled(&m_tree->node, true);
      setAlpha(0.0F);
      if (m_fadeAnim != 0) {
        m_server->animator().cancel(m_fadeAnim);
      }
      m_fadeAnim = m_server->animator().animate(
          0.0, kHintAlpha, kAnimMs, [this](double alpha) { setAlpha(static_cast<float>(alpha)); },
          [this] { m_fadeAnim = 0; }
      );
      m_visible = true;
    } else if (m_tree->node.x != x || m_tree->node.y != y) {
      const int fromX = m_tree->node.x;
      const int fromY = m_tree->node.y;
      m_positionAnim = m_server->animator().animate(
          0.0, 1.0, kAnimMs,
          [this, fromX, fromY, x, y](double progress) {
            wlr_scene_node_set_position(
                &m_tree->node, static_cast<int>(std::lround(fromX + (x - fromX) * progress)),
                static_cast<int>(std::lround(fromY + (y - fromY) * progress))
            );
          },
          [this] { m_positionAnim = 0; }
      );
    } else {
      wlr_scene_node_set_position(&m_tree->node, x, y);
    }
    scheduleFrame(workspace);
  }

  void InsertHint::hide() {
    if (m_positionAnim != 0) {
      m_server->animator().cancel(m_positionAnim);
      m_positionAnim = 0;
    }
    if (m_fadeAnim != 0) {
      m_server->animator().cancel(m_fadeAnim);
      m_fadeAnim = 0;
    }
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    m_visible = false;
  }

} // namespace umbriel
