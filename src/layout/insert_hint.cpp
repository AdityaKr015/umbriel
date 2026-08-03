#include "layout/insert_hint.h"

#include "config/config.h"
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
    constexpr float kHintAlpha = 0.28F;
    constexpr float kShadowAlpha = 0.10F;
    constexpr int kHintFadeMs = 80;

    // SceneFX rect/shadow colors are premultiplied.
    void premultiplied(float out[4], float alpha) {
      out[0] = kHintRed * alpha;
      out[1] = kHintGreen * alpha;
      out[2] = kHintBlue * alpha;
      out[3] = alpha;
    }
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
    // dragTree sits above xdg windows so the hint is never covered by them.
    m_tree = wlr_scene_tree_create(m_server->dragTree());
    float shadowColor[4]{};
    premultiplied(shadowColor, kShadowAlpha);
    m_shadow = wlr_scene_shadow_create(m_tree, kHintWidth, 1, kHintWidth / 2, 18.0F, shadowColor);
    float rectColor[4]{};
    premultiplied(rectColor, 0.0F);
    m_rect = wlr_scene_rect_create(m_tree, kHintWidth, 1, rectColor);
    wlr_scene_rect_set_corner_radius(m_rect, kHintWidth / 2);
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  void InsertHint::setAlpha(float alpha) {
    float color[4]{};
    premultiplied(color, alpha);
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
    const int viewportWidth = std::max(1, usable.width - 2 * config().layout.gap);
    const int columnCount = static_cast<int>(workspace->layout().columns().size());
    const int gap = std::clamp(gapIndex, 0, columnCount);
    // Paint onto the column boundary (overlay), do not open a layout gap.
    int boundaryX = 0;
    if (columnCount == 0) {
      boundaryX = 0;
    } else if (gap <= 0) {
      boundaryX = 0;
    } else if (gap >= columnCount) {
      boundaryX = workspace->layout().columnX(columnCount - 1, viewportWidth)
          + workspace->layout().columnWidth(columnCount - 1, viewportWidth);
    } else {
      boundaryX = workspace->layout().columnX(gap, viewportWidth) - config().layout.gap / 2;
    }
    const int targetX = usable.x
        + config().layout.gap
        + boundaryX
        - kHintWidth / 2
        - static_cast<int>(std::lround(workspace->visualScroll()));
    showGeometry(
        workspace, targetX, usable.y + config().layout.gap, kHintWidth,
        std::max(1, usable.height - 2 * config().layout.gap)
    );
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
    const int viewportWidth = std::max(1, usable.width - 2 * config().layout.gap);
    const Column& column = workspace->layout().columns()[static_cast<size_t>(columnIndex)];
    const int row = std::clamp(rowIndex, 0, static_cast<int>(column.views.size()));
    int boundaryY = usable.y + config().layout.gap;
    if (row == static_cast<int>(column.views.size())) {
      boundaryY = usable.y + usable.height - config().layout.gap;
    } else if (row > 0) {
      boundaryY = workspace->layout().targetBox(column.views[static_cast<size_t>(row)]).y - config().layout.gap / 2;
    }
    const int targetX = usable.x
        + config().layout.gap
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

    // Snap to the drop target; animating the indicator lags behind the cursor.
    wlr_scene_node_set_position(&m_tree->node, x, y);
    wlr_scene_node_raise_to_top(&m_tree->node);

    if (!m_visible) {
      wlr_scene_node_set_enabled(&m_tree->node, true);
      setAlpha(0.0F);
      if (m_fadeAnim != 0) {
        m_server->animator().cancel(m_fadeAnim);
      }
      m_fadeAnim = m_server->animator().animate(
          0.0, kHintAlpha, kHintFadeMs, [this](double alpha) { setAlpha(static_cast<float>(alpha)); },
          [this] { m_fadeAnim = 0; }
      );
      m_visible = true;
    } else {
      setAlpha(kHintAlpha);
    }
    scheduleFrame(workspace);
  }

  void InsertHint::hide() {
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
