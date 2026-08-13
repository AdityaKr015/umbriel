#include "layout/drop_target.h"

#include "config/config.h"
#include "layout/dwindle.h"
#include "layout/layout.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "server/server.h"
#include "view/view.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {
    constexpr int kColumnHintWidth = 300;
    constexpr int kRowHintEdgeHeight = 150;
    constexpr int kRowHintMidHeight = 300;
    constexpr int kScrollingEdgeDropWidth = 32;

    struct ScrollingTarget {
      int column = 0;
      int row = -1;
    };

    struct DwindleTarget {
      View* view = nullptr;
      uint32_t edge = 0;
      int leaf = -1;
      wlr_box hint{};
    };

    wlr_box columnHintBox(
        const Workspace& workspace, const ScrollingLayout& layout, const wlr_box& usable, int gapIndex, double scroll
    ) {
      const int edgePad = workspace.layoutConfig().edgePad;
      const int gap = workspace.layoutConfig().totalGap;
      const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
      const int columnCount = static_cast<int>(workspace.layout().columns().size());
      const int clampedGap = std::clamp(gapIndex, 0, columnCount);

      int hintX = 0;
      if (columnCount == 0) {
        hintX = 0;
      } else if (clampedGap <= 0) {
        hintX = 0;
      } else if (clampedGap >= columnCount) {
        hintX =
            layout.columnX(columnCount - 1, viewportWidth) + layout.columnWidth(columnCount - 1, viewportWidth) + gap;
      } else {
        hintX = layout.columnX(clampedGap, viewportWidth) - gap / 2 - kColumnHintWidth / 2;
      }

      return {
          .x = usable.x + edgePad + hintX - static_cast<int>(std::lround(scroll)),
          .y = usable.y + edgePad,
          .width = kColumnHintWidth,
          .height = std::max(1, usable.height - 2 * edgePad),
      };
    }

    wlr_box stackHintBox(
        const Workspace& workspace, const ScrollingLayout& layout, const wlr_box& usable, int columnIndex, int rowIndex,
        double scroll
    ) {
      if (columnIndex < 0 || columnIndex >= static_cast<int>(workspace.layout().columns().size())) {
        return {};
      }
      const int edgePad = workspace.layoutConfig().edgePad;
      const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
      const Column& column = workspace.layout().columns()[static_cast<size_t>(columnIndex)];
      const int rowCount = static_cast<int>(column.views.size());
      const int row = std::clamp(rowIndex, 0, rowCount);

      int hintY = 0;
      int hintHeight = 0;
      if (row == 0) {
        hintY = usable.y + edgePad;
        hintHeight = kRowHintEdgeHeight;
      } else if (row >= rowCount) {
        hintY = usable.y + usable.height - edgePad - kRowHintEdgeHeight;
        hintHeight = kRowHintEdgeHeight;
      } else {
        const int boundary = workspace.layout().targetBox(column.views[static_cast<size_t>(row)]).y
            - workspace.layoutConfig().totalGap / 2;
        hintY = boundary - kRowHintMidHeight / 2;
        hintHeight = kRowHintMidHeight;
      }

      return {
          .x = usable.x + edgePad + layout.columnX(columnIndex, viewportWidth) - static_cast<int>(std::lround(scroll)),
          .y = hintY,
          .width = layout.columnWidth(columnIndex, viewportWidth),
          .height = hintHeight,
      };
    }

    wlr_box clampHintBox(const wlr_box& box, const wlr_box& usable) {
      const int clampedX = std::max(box.x, usable.x);
      const int clampedY = std::max(box.y, usable.y);
      const int clampedX2 = std::min(box.x + box.width, usable.x + usable.width);
      const int clampedY2 = std::min(box.y + box.height, usable.y + usable.height);
      if (clampedX >= clampedX2 || clampedY >= clampedY2) {
        return {};
      }
      return {
          .x = clampedX,
          .y = clampedY,
          .width = clampedX2 - clampedX,
          .height = clampedY2 - clampedY,
      };
    }

    DwindleTarget
    computeDwindleTarget(const DwindleLayout& layout, double worldX, double worldY, const View* excludedView) {
      DwindleTarget result{};
      result.leaf = layout.leafIndexAt(worldX, worldY);
      if (result.leaf < 0) {
        return result;
      }

      const wlr_box targetBox = layout.targetBoxByIndex(result.leaf);
      const auto& leaves = layout.columns();
      if (targetBox.width <= 0
          || targetBox.height <= 0
          || result.leaf >= static_cast<int>(leaves.size())
          || leaves[static_cast<size_t>(result.leaf)].views.empty()) {
        return result;
      }

      result.view = leaves[static_cast<size_t>(result.leaf)].views.front();
      if (result.view == excludedView) {
        result.view = nullptr;
        return result;
      }

      const double fx = (worldX - targetBox.x) / static_cast<double>(targetBox.width);
      const double fy = (worldY - targetBox.y) / static_cast<double>(targetBox.height);
      result.hint = targetBox;
      if (std::min(fx, 1.0 - fx) <= std::min(fy, 1.0 - fy)) {
        if (fx <= 0.5) {
          result.edge = WLR_EDGE_LEFT;
          result.hint.width = targetBox.width / 2;
        } else {
          result.edge = WLR_EDGE_RIGHT;
          result.hint.x = targetBox.x + targetBox.width / 2;
          result.hint.width = targetBox.width - targetBox.width / 2;
        }
      } else if (fy <= 0.5) {
        result.edge = WLR_EDGE_TOP;
        result.hint.height = targetBox.height / 2;
      } else {
        result.edge = WLR_EDGE_BOTTOM;
        result.hint.y = targetBox.y + targetBox.height / 2;
        result.hint.height = targetBox.height - targetBox.height / 2;
      }
      return result;
    }
    bool atScrollingRightEdge(const wlr_box& usable, double worldX) {
      return worldX >= static_cast<double>(usable.x + usable.width - kScrollingEdgeDropWidth);
    }

    ScrollingTarget computeScrollingTarget(
        const Workspace& workspace, const ScrollingLayout& layout, const wlr_box& usable, double scroll, double worldX,
        double worldY
    ) {
      const int edgePad = workspace.layoutConfig().edgePad;
      const int totalGap = workspace.layoutConfig().totalGap;
      const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
      const int columnCount = static_cast<int>(layout.columns().size());
      // The strip may extend far beyond the viewport, making its final
      // layout-space gap unreachable. Reserve the output's extreme right edge
      // as a stable append target regardless of the current scroll offset.
      if (columnCount > 0 && layout.maxScroll(viewportWidth) > 0 && atScrollingRightEdge(usable, worldX)) {
        return {.column = columnCount, .row = -1};
      }

      const double layoutX = worldX - usable.x - edgePad + scroll;

      for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
        const int columnX = layout.columnX(columnIndex, viewportWidth);
        const int columnWidth = layout.columnWidth(columnIndex, viewportWidth);
        if (layoutX < columnX + columnWidth * 0.2 || layoutX > columnX + columnWidth * 0.8) {
          continue;
        }
        const Column& column = layout.columns()[static_cast<size_t>(columnIndex)];
        int nearestRow = 0;
        double rowDistance = std::abs(worldY - (usable.y + edgePad));
        for (int row = 1; row <= static_cast<int>(column.views.size()); ++row) {
          const int boundary = row == static_cast<int>(column.views.size())
              ? usable.y + usable.height - edgePad
              : layout.targetBox(column.views[static_cast<size_t>(row)]).y - totalGap / 2;
          const double distance = std::abs(worldY - boundary);
          if (distance < rowDistance) {
            nearestRow = row;
            rowDistance = distance;
          }
        }
        return {.column = columnIndex, .row = nearestRow};
      }

      int nearestGap = 0;
      double nearestDistance = std::abs(layoutX);
      for (int gap = 1; gap <= columnCount; ++gap) {
        const int boundary = gap == columnCount ? layout.columnX(gap, viewportWidth) - totalGap
                                                : layout.columnX(gap, viewportWidth) - totalGap / 2;
        const double distance = std::abs(layoutX - boundary);
        if (distance < nearestDistance) {
          nearestGap = gap;
          nearestDistance = distance;
        }
      }
      return {.column = nearestGap, .row = -1};
    }
  } // namespace
  std::optional<DropColumnWidth> captureDropColumnWidth(const Workspace& source, const View* view) {
    const ScrollingLayout* scrolling = source.scrollingLayout();
    if (scrolling == nullptr || view == nullptr) {
      return std::nullopt;
    }
    const int columnIndex = scrolling->columnOf(view);
    const auto& columns = scrolling->columns();
    if (columnIndex < 0 || columnIndex >= static_cast<int>(columns.size())) {
      return std::nullopt;
    }
    const Column& column = columns[static_cast<size_t>(columnIndex)];
    if (column.views.size() != 1) {
      return std::nullopt;
    }
    return DropColumnWidth{
        .fraction = column.savedWidthFrac > 0.0 ? column.savedWidthFrac : column.widthFrac,
        .fullWidth = column.savedWidthFrac > 0.0,
    };
  }

  DropTarget computeDropTarget(Workspace& workspace, double worldX, double worldY, const View* excludedView) {
    DropTarget result{.workspace = &workspace};
    if (workspace.group() == nullptr || workspace.group()->output() == nullptr) {
      return result;
    }
    const wlr_box usable = workspace.group()->output()->usableArea();
    if (usable.width <= 0 || usable.height <= 0) {
      return result;
    }

    // Layout mode is a genuine policy fork here, not a capability check: the two
    // layouts want different drop targets and different hint shapes.
    if (DwindleLayout* dwindle = workspace.dwindleLayout()) {
      const DwindleTarget target = computeDwindleTarget(*dwindle, worldX, worldY, excludedView);
      result.column = target.leaf >= 0 ? target.leaf : static_cast<int>(workspace.layout().columns().size());
      result.view = target.view;
      result.edge = target.edge;
      if (target.view != nullptr && target.edge != 0) {
        result.hintBox = target.hint;
      }
    } else if (const ScrollingLayout* scrolling = workspace.scrollingLayout()) {
      const ScrollingLayout& layout = *scrolling;
      const double scroll = layout.scroll();
      const ScrollingTarget target = computeScrollingTarget(workspace, layout, usable, scroll, worldX, worldY);
      result.column = target.column;
      result.row = target.row;
      if (target.row >= 0) {
        result.hintBox = stackHintBox(workspace, layout, usable, target.column, target.row, scroll);
      } else if (target.column == static_cast<int>(layout.columns().size()) && atScrollingRightEdge(usable, worldX)) {
        result.hintBox = {
            .x = usable.x + usable.width - kColumnHintWidth,
            .y = usable.y + workspace.layoutConfig().edgePad,
            .width = kColumnHintWidth,
            .height = std::max(1, usable.height - 2 * workspace.layoutConfig().edgePad),
        };
      } else {
        result.hintBox = columnHintBox(workspace, layout, usable, target.column, scroll);
      }
    } else {
      result.column = static_cast<int>(workspace.layout().columns().size());
    }

    result.hintBox = clampHintBox(result.hintBox, usable);
    return result;
  }

  void applyDrop(
      Server& server, View& view, Workspace& target, const DropTarget& drop, const DropColumnWidth* columnWidth,
      bool animate
  ) {
    if (server.scratchpadManager() != nullptr && server.scratchpadManager()->contains(&view)) {
      return;
    }
    // Policy fork again: a dwindle drop splits a leaf, a scrolling drop inserts
    // a column.
    if (DwindleLayout* dwindle = target.dwindleLayout()) {
      const bool splitDrop =
          drop.view != nullptr && drop.view != &view && drop.edge != 0 && dwindle->columnOf(drop.view) >= 0;
      if (view.workspace() != &target) {
        // Auto-attach would split the focused leaf and send a stale configure
        // before the explicit placement below.
        view.setWorkspace(&target, /*attachToLayout=*/false);
      } else {
        dwindle->removeView(&view);
      }
      if (splitDrop) {
        dwindle->insertViewSplitOnView(&view, drop.view, drop.edge);
      } else {
        dwindle->insertView(&view, static_cast<int>(dwindle->columns().size()));
      }
      wlr_scene_node_reparent(&view.sceneTree()->node, target.viewLayer(true));
      target.markArrange(animate);
      server.focusView(&view, FocusReason::DragDrop);
      return;
    }

    if (view.workspace() != &target) {
      view.setWorkspace(&target, /*attachToLayout=*/false);
    }
    if (drop.row >= 0) {
      target.layout().insertViewIntoColumn(&view, std::max(0, drop.column), drop.row);
    } else {
      target.layout().insertView(&view, std::max(0, drop.column));
    }
    if (columnWidth != nullptr && drop.row < 0) {
      const int column = target.layout().columnOf(&view);
      target.layout().setWidthFraction(column, columnWidth->fraction);
      if (columnWidth->fullWidth) {
        target.layout().toggleFullWidth(column);
      }
      wlr_xdg_toplevel_set_maximized(view.toplevel(), columnWidth->fullWidth);
    }
    wlr_scene_node_reparent(&view.sceneTree()->node, target.viewLayer(true));
    target.markArrange(animate);
    server.focusView(&view, FocusReason::DragDrop);
  }

} // namespace umbriel
