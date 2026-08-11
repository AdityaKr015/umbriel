#include "layout/insert_hint.h"

#include "config/config.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "scene/color.h"
#include "server/server.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    // Width of the column-insert preview rectangle (logical pixels).
    // Matches the layout-gap reservation in ScrollingLayout::columnX.
    constexpr int kColumnHintWidth = 300;

    // Height of the row-insert preview when at an edge (first/last), and full
    // height when inserting between two existing rows.
    constexpr int kRowHintEdgeHeight = 150;
    constexpr int kRowHintMidHeight = 300;

  } // namespace

  InsertHint::InsertHint(Server& server) : m_server(&server) {}

  InsertHint::~InsertHint() {
    hide();
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
      m_rect = nullptr;
    }
  }

  void InsertHint::ensureScene() {
    if (m_tree != nullptr) {
      return;
    }
    // dragTree sits above xdg windows so the hint is never covered by them.
    m_tree = wlr_scene_tree_create(m_server->dragTree());
    float color[4]{};
    premultiplied(color, config().appearance.insertHintColor, 0.0F);
    m_rect = wlr_scene_rect_create(m_tree, 1, 1, color);
    wlr_scene_rect_set_corner_radius(m_rect, config().appearance.cornerRadius);
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  void InsertHint::setAlpha(float alpha) {
    float color[4]{};
    premultiplied(color, config().appearance.insertHintColor, alpha);
    wlr_scene_rect_set_color(m_rect, color);
  }

  void InsertHint::scheduleFrame(Workspace* workspace) const {
    if (workspace != nullptr && workspace->group() != nullptr && workspace->group()->output() != nullptr) {
      wlr_output_schedule_frame(workspace->group()->output()->wlr());
    }
  }

  wlr_box InsertHint::gapHintBox(const Workspace& workspace, int gapIndex, double scroll) {
    if (workspace.group() == nullptr
        || workspace.group()->output() == nullptr
        || workspace.layoutMode() != LayoutMode::Scrolling) {
      return {};
    }
    const wlr_box usable = workspace.group()->output()->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return {};
    }
    const int edgePad = workspace.layoutConfig().edgePad;
    const int gap = workspace.layoutConfig().totalGap;
    const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
    const int columnCount = static_cast<int>(workspace.layout().columns().size());
    const int clampedGap = std::clamp(gapIndex, 0, columnCount);

    // Position the preview rectangle at the would-be column location.
    int hintX = 0;
    if (columnCount == 0) {
      // Empty workspace: center the hint.
      hintX = 0;
    } else if (clampedGap <= 0) {
      // Before the first column: flush with the left edge of the layout.
      // The viewport cannot scroll to negative offsets, so the previous
      // position (-kColumnHintWidth - gap) fell entirely outside the
      // usable area and was clipped away by showGeometry.
      hintX = 0;
    } else if (clampedGap >= columnCount) {
      // After the last column.
      hintX = workspace.layout().columnX(columnCount - 1, viewportWidth)
          + workspace.layout().columnWidth(columnCount - 1, viewportWidth)
          + gap;
    } else {
      // Between two columns: center on the gap boundary.
      hintX = workspace.layout().columnX(clampedGap, viewportWidth) - gap / 2 - kColumnHintWidth / 2;
    }

    return {
        .x = usable.x + edgePad + hintX - static_cast<int>(std::lround(scroll)),
        .y = usable.y + edgePad,
        .width = kColumnHintWidth,
        .height = std::max(1, usable.height - 2 * edgePad),
    };
  }

  wlr_box InsertHint::rowHintBox(const Workspace& workspace, int columnIndex, int rowIndex, double scroll) {
    if (workspace.group() == nullptr
        || workspace.group()->output() == nullptr
        || workspace.layoutMode() != LayoutMode::Scrolling
        || columnIndex < 0
        || columnIndex >= static_cast<int>(workspace.layout().columns().size())) {
      return {};
    }
    const wlr_box usable = workspace.group()->output()->usableArea();
    const int edgePad = workspace.layoutConfig().edgePad;
    const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
    const Column& column = workspace.layout().columns()[static_cast<size_t>(columnIndex)];
    const int rowCount = static_cast<int>(column.views.size());
    const int row = std::clamp(rowIndex, 0, rowCount);

    // Compute the preview rectangle anchored at the row boundary.
    int hintY = 0;
    int hintHeight = 0;
    if (row == 0) {
      // At the top: anchor to the top edge.
      hintY = usable.y + edgePad;
      hintHeight = kRowHintEdgeHeight;
    } else if (row >= rowCount) {
      // At the bottom: anchor to the bottom edge.
      hintY = usable.y + usable.height - edgePad - kRowHintEdgeHeight;
      hintHeight = kRowHintEdgeHeight;
    } else {
      // Between two rows: centered on the boundary.
      const int boundary = workspace.layout().targetBox(column.views[static_cast<size_t>(row)]).y
          - workspace.layoutConfig().totalGap / 2;
      hintY = boundary - kRowHintMidHeight / 2;
      hintHeight = kRowHintMidHeight;
    }

    return {
        .x = usable.x
            + edgePad
            + workspace.layout().columnX(columnIndex, viewportWidth)
            - static_cast<int>(std::lround(scroll)),
        .y = hintY,
        .width = workspace.layout().columnWidth(columnIndex, viewportWidth),
        .height = hintHeight,
    };
  }

  void InsertHint::show(Workspace* workspace, int gapIndex) {
    if (workspace == nullptr) {
      return;
    }
    const wlr_box box = gapHintBox(*workspace, gapIndex, workspace->layout().scroll());
    if (box.width <= 0 || box.height <= 0) {
      return;
    }
    showGeometry(workspace, box.x, box.y, box.width, box.height);
  }

  void InsertHint::showRow(Workspace* workspace, int columnIndex, int rowIndex) {
    if (workspace == nullptr) {
      return;
    }
    const wlr_box box = rowHintBox(*workspace, columnIndex, rowIndex, workspace->layout().scroll());
    if (box.width <= 0 || box.height <= 0) {
      return;
    }
    showGeometry(workspace, box.x, box.y, box.width, box.height);
  }

  void InsertHint::showBox(Workspace* workspace, const wlr_box& geometry) {
    if (workspace == nullptr || workspace->group() == nullptr || workspace->group()->output() == nullptr) {
      return;
    }
    showGeometry(workspace, geometry.x, geometry.y, geometry.width, geometry.height);
  }

  void InsertHint::showGeometry(Workspace* workspace, int x, int y, int width, int height) {
    ensureScene();

    // Clamp the hint rectangle to the output's usable area so it never bleeds
    // onto an adjacent monitor (e.g. inserting at the first column of a screen
    // that sits to the right of another).
    const wlr_box usable = workspace->group()->output()->usableArea();
    const int x2 = x + width;
    const int y2 = y + height;
    const int clampedX = std::max(x, usable.x);
    const int clampedY = std::max(y, usable.y);
    const int clampedX2 = std::min(x2, usable.x + usable.width);
    const int clampedY2 = std::min(y2, usable.y + usable.height);
    if (clampedX >= clampedX2 || clampedY >= clampedY2) {
      hide();
      return;
    }
    x = clampedX;
    y = clampedY;
    width = clampedX2 - clampedX;
    height = clampedY2 - clampedY;

    wlr_scene_rect_set_size(m_rect, width, height);
    const int radius = config().appearance.cornerRadius;
    wlr_scene_rect_set_corner_radius(m_rect, radius);

    // Snap to the drop target; animating the indicator lags behind the cursor.
    wlr_scene_node_set_position(&m_tree->node, x, y);
    wlr_scene_node_raise_to_top(&m_tree->node);

    if (!m_visible) {
      wlr_scene_node_set_enabled(&m_tree->node, true);
      setAlpha(0.0F);
      m_fade.snap(0.0);
      // Same duration as the other appear-fades (map fade-in, close snapshot).
      m_fade.retarget(1.0, std::max(1, config().appearance.animationMs / 2), Easing::EaseOutCubic);
      m_visible = true;
    } else if (!m_fade.animating()) {
      // Only force full alpha once the fade is done: drag motion re-enters here
      // every event, and overriding a running fade flickers the hint.
      setAlpha(1.0F);
    }
    scheduleFrame(workspace);
  }

  void InsertHint::hide() {
    m_fade.snap(1.0);
    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    m_visible = false;
  }

  bool InsertHint::tickAnimations(uint64_t nowMsec) {
    if (!m_fade.tick(nowMsec)) {
      return false;
    }
    setAlpha(static_cast<float>(m_fade.current()));
    return m_fade.animating();
  }

} // namespace umbriel
