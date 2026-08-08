#include "layout/drop_target.h"

#include "layout/layout.h"
#include "workspace/workspace.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  ScrollingDropTarget computeScrollingDropTarget(
      const Workspace& workspace, const wlr_box& usable, double scroll, double worldX, double worldY
  ) {
    const Layout& layout = workspace.layout();
    const int edgePad = workspace.layoutConfig().edgePad;
    const int totalGap = workspace.layoutConfig().totalGap;
    const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
    const int columnCount = static_cast<int>(layout.columns().size());
    const double layoutX = worldX - usable.x - edgePad + scroll;

    // Middle 60% of a column targets a row inside it; the outer bands fall
    // through to the gap scan below so a new column can be opened there.
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

} // namespace umbriel
