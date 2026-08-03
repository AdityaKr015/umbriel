#include "layout/scrolling.h"

#include "config/config.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {

    constexpr double kMinHeightWeight = 0.05;

    void ensureWeightCount(Column& column) {
      while (column.heightWeights.size() < column.views.size()) {
        column.heightWeights.push_back(1.0);
      }
      if (column.heightWeights.size() > column.views.size()) {
        column.heightWeights.resize(column.views.size());
      }
    }

    double columnTotalWeight(const Column& column) {
      double total = std::max(0.0, column.topGapWeight) + std::max(0.0, column.bottomGapWeight);
      for (double weight : column.heightWeights) {
        total += std::max(kMinHeightWeight, weight);
      }
      return std::max(kMinHeightWeight, total);
    }

  } // namespace

  void ScrollingLayout::syncHeightWeights(Column& column) { ensureWeightCount(column); }

  int ScrollingLayout::columnOf(const View* view) const {
    for (size_t i = 0; i < m_columns.size(); ++i) {
      if (std::find(m_columns[i].views.begin(), m_columns[i].views.end(), view) != m_columns[i].views.end()) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int ScrollingLayout::rowOf(const View* view) const {
    const int column = columnOf(view);
    if (column < 0) {
      return -1;
    }
    const auto& views = m_columns[static_cast<size_t>(column)].views;
    const auto it = std::find(views.begin(), views.end(), view);
    return it == views.end() ? -1 : static_cast<int>(it - views.begin());
  }

  int ScrollingLayout::columnWidth(int columnIndex, int viewportWidth) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 0;
    }
    return std::clamp(
        static_cast<int>(std::lround(m_columns[static_cast<size_t>(columnIndex)].widthFrac * viewportWidth)), 1,
        std::max(1, viewportWidth)
    );
  }

  int ScrollingLayout::columnX(int columnIndex, int viewportWidth) const {
    const int end = std::clamp(columnIndex, 0, static_cast<int>(m_columns.size()));
    const int gap = config().layoutGap();
    int x = 0;
    for (int i = 0; i < end; ++i) {
      x += columnWidth(i, viewportWidth) + gap;
    }
    if (m_insertGap >= 0 && end >= m_insertGap) {
      x += kHintWidth + gap;
    }
    return x;
  }

  int ScrollingLayout::totalWidth(int viewportWidth) const {
    if (m_columns.empty()) {
      return 0;
    }
    return columnX(static_cast<int>(m_columns.size()), viewportWidth) - config().layoutGap();
  }

  bool ScrollingLayout::isFullWidth(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    return m_columns[static_cast<size_t>(columnIndex)].savedWidthFrac > 0.0;
  }

  void ScrollingLayout::insertView(View* view, int columnIndex) {
    if (view == nullptr || columnOf(view) >= 0) {
      return;
    }
    const int index = std::clamp(columnIndex, 0, static_cast<int>(m_columns.size()));
    Column column;
    column.views.push_back(view);
    column.heightWeights.push_back(1.0);
    m_columns.insert(m_columns.begin() + index, std::move(column));
  }

  void ScrollingLayout::insertViewIntoColumn(View* view, int columnIndex, int rowIndex) {
    if (view == nullptr
        || columnOf(view) >= 0
        || columnIndex < 0
        || columnIndex >= static_cast<int>(m_columns.size())) {
      return;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    const int row = std::clamp(rowIndex, 0, static_cast<int>(column.views.size()));
    column.views.insert(column.views.begin() + row, view);
    column.heightWeights.insert(column.heightWeights.begin() + row, 1.0);
  }

  bool ScrollingLayout::consumeLeft(View* view) {
    const int sourceColumn = columnOf(view);
    if (sourceColumn <= 0) {
      return false;
    }
    Column& source = m_columns[static_cast<size_t>(sourceColumn)];
    Column& dest = m_columns[static_cast<size_t>(sourceColumn - 1)];
    ensureWeightCount(source);
    ensureWeightCount(dest);
    const int row = rowOf(view);
    const double weight = row >= 0 ? source.heightWeights[static_cast<size_t>(row)] : 1.0;
    std::erase(source.views, view);
    if (row >= 0 && row < static_cast<int>(source.heightWeights.size())) {
      source.heightWeights.erase(source.heightWeights.begin() + row);
    }
    dest.views.push_back(view);
    dest.heightWeights.push_back(weight);
    if (source.views.empty()) {
      m_columns.erase(m_columns.begin() + sourceColumn);
    }
    return true;
  }

  bool ScrollingLayout::expelRight(View* view) {
    const int sourceColumn = columnOf(view);
    if (sourceColumn < 0) {
      return false;
    }
    Column& source = m_columns[static_cast<size_t>(sourceColumn)];
    if (source.views.size() <= 1) {
      return false;
    }
    ensureWeightCount(source);
    const int row = rowOf(view);
    const double weight = row >= 0 ? source.heightWeights[static_cast<size_t>(row)] : 1.0;
    std::erase(source.views, view);
    if (row >= 0 && row < static_cast<int>(source.heightWeights.size())) {
      source.heightWeights.erase(source.heightWeights.begin() + row);
    }
    Column column;
    column.views.push_back(view);
    column.heightWeights.push_back(weight);
    m_columns.insert(m_columns.begin() + sourceColumn + 1, std::move(column));
    return true;
  }

  bool ScrollingLayout::moveViewVertical(View* view, int direction) {
    const int column = columnOf(view);
    const int row = rowOf(view);
    if (column < 0 || row < 0) {
      return false;
    }
    Column& col = m_columns[static_cast<size_t>(column)];
    ensureWeightCount(col);
    const int target = row + direction;
    if (target < 0 || target >= static_cast<int>(col.views.size())) {
      return false;
    }
    std::swap(col.views[static_cast<size_t>(row)], col.views[static_cast<size_t>(target)]);
    std::swap(col.heightWeights[static_cast<size_t>(row)], col.heightWeights[static_cast<size_t>(target)]);
    return true;
  }

  void ScrollingLayout::removeView(View* view) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    const int row = rowOf(view);
    std::erase(column.views, view);
    if (row >= 0 && row < static_cast<int>(column.heightWeights.size())) {
      column.heightWeights.erase(column.heightWeights.begin() + row);
    }
    if (column.views.empty()) {
      m_columns.erase(m_columns.begin() + columnIndex);
    }
    std::erase_if(m_targets, [view](const Target& target) { return target.view == view; });
  }

  void ScrollingLayout::moveColumn(int from, int to) {
    if (from < 0 || from >= static_cast<int>(m_columns.size())) {
      return;
    }
    const int destination = std::clamp(to, 0, static_cast<int>(m_columns.size()) - 1);
    if (from == destination) {
      return;
    }
    Column column = std::move(m_columns[static_cast<size_t>(from)]);
    m_columns.erase(m_columns.begin() + from);
    m_columns.insert(m_columns.begin() + destination, std::move(column));
  }

  void ScrollingLayout::setInsertGap(int gapIndex) {
    m_insertGap = std::clamp(gapIndex, 0, static_cast<int>(m_columns.size()));
  }

  void ScrollingLayout::clearInsertGap() { m_insertGap = -1; }

  void ScrollingLayout::setScroll(double scroll) { m_scroll = scroll; }

  double ScrollingLayout::targetScrollForEnsureVisible(int columnIndex, int viewportWidth) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size()) || viewportWidth <= 0) {
      return m_scroll;
    }
    const int x = columnX(columnIndex, viewportWidth);
    const int width = columnWidth(columnIndex, viewportWidth);
    // Full-width (Super+F) fills the viewport: no neighbor peek, so sloppy focus
    // cannot land on a peek strip of another column.
    if (isFullWidth(columnIndex) || width >= viewportWidth) {
      const double scroll = static_cast<double>(x);
      const double max = static_cast<double>(std::max(0, totalWidth(viewportWidth) - viewportWidth));
      return std::clamp(scroll, 0.0, max);
    }

    // Leave a strip of each neighbor on-screen so the pointer can reach it.
    const int peek = std::max(config().layout.gap * 2, 32);
    const bool hasPrev = columnIndex > 0;
    const bool hasNext = columnIndex + 1 < static_cast<int>(m_columns.size());

    double minScroll = static_cast<double>(x + width - viewportWidth);
    double maxScroll = static_cast<double>(x);
    if (hasNext) {
      minScroll += static_cast<double>(peek);
    }
    if (hasPrev) {
      maxScroll -= static_cast<double>(peek);
    }

    double scroll = m_scroll;
    if (minScroll > maxScroll) {
      if (hasPrev && !hasNext) {
        scroll = maxScroll;
      } else if (hasNext && !hasPrev) {
        scroll = minScroll;
      } else if (hasPrev && hasNext) {
        scroll = m_scroll <= static_cast<double>(x) ? maxScroll : minScroll;
      } else {
        scroll = std::clamp(m_scroll, static_cast<double>(x + width - viewportWidth), static_cast<double>(x));
      }
    } else {
      scroll = std::clamp(m_scroll, minScroll, maxScroll);
    }
    const double max = static_cast<double>(std::max(0, totalWidth(viewportWidth) - viewportWidth));
    return std::clamp(scroll, 0.0, max);
  }

  double ScrollingLayout::scrollAmountToEnsureVisible(int columnIndex, int viewportWidth) const {
    if (viewportWidth <= 0) {
      return 0.0;
    }
    return std::abs(targetScrollForEnsureVisible(columnIndex, viewportWidth) - m_scroll)
        / static_cast<double>(viewportWidth);
  }

  void ScrollingLayout::ensureVisible(int columnIndex, int viewportWidth) {
    m_scroll = targetScrollForEnsureVisible(columnIndex, viewportWidth);
  }

  void ScrollingLayout::arrange(const wlr_box& usable) {
    m_targets.clear();
    // Include outer border in the usable-area inset so decorations stay clear of
    // layer-shell exclusive zones (panels).
    const int edgePad = config().layoutEdgePad();
    const int viewportWidth = std::max(1, usable.width - 2 * edgePad);
    const int availableHeight = std::max(1, usable.height - 2 * edgePad);
    const double maxScroll = static_cast<double>(std::max(0, totalWidth(viewportWidth) - viewportWidth));
    // Allow slightly negative scroll so column-0 left-edge resize can pin the right edge
    // when shrinking would otherwise leave empty space that clamp-to-0 would eat.
    m_scroll = std::clamp(m_scroll, -static_cast<double>(viewportWidth), maxScroll);

    for (size_t columnIndex = 0; columnIndex < m_columns.size(); ++columnIndex) {
      Column& column = m_columns[columnIndex];
      if (column.views.empty()) {
        continue;
      }
      ensureWeightCount(column);
      const int width = columnWidth(static_cast<int>(columnIndex), viewportWidth);
      const int x = usable.x
          + edgePad
          + columnX(static_cast<int>(columnIndex), viewportWidth)
          - static_cast<int>(std::lround(m_scroll));
      const int rowCount = static_cast<int>(column.views.size());
      const int gap = config().layoutGap();
      const int gapsTotal = std::max(0, rowCount - 1) * gap;
      const int stackHeight = std::max(rowCount, availableHeight - gapsTotal);
      const double totalWeight = columnTotalWeight(column);

      int y = usable.y + edgePad;
      const int topGapPx =
          static_cast<int>(std::lround(std::max(0.0, column.topGapWeight) / totalWeight * stackHeight));
      y += topGapPx;
      int used = topGapPx;

      for (int row = 0; row < rowCount; ++row) {
        const double weight = std::max(kMinHeightWeight, column.heightWeights[static_cast<size_t>(row)]);
        int height = static_cast<int>(std::lround(weight / totalWeight * stackHeight));
        if (row == rowCount - 1) {
          const int bottomGapPx =
              static_cast<int>(std::lround(std::max(0.0, column.bottomGapWeight) / totalWeight * stackHeight));
          height = std::max(1, stackHeight - used - bottomGapPx);
        } else {
          height = std::max(1, height);
        }
        m_targets.push_back(
            {.view = column.views[static_cast<size_t>(row)], .x = x, .y = y, .width = width, .height = height}
        );
        y += height + gap;
        used += height;
      }
    }
  }

  wlr_box ScrollingLayout::targetBox(const View* view) const {
    const auto it =
        std::find_if(m_targets.begin(), m_targets.end(), [view](const Target& target) { return target.view == view; });
    if (it == m_targets.end()) {
      return {};
    }
    return {.x = it->x, .y = it->y, .width = it->width, .height = it->height};
  }

  bool ScrollingLayout::cycleWidth(int columnIndex) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    const auto& presets = config().layout.widthPresets;
    const auto it = std::find_if(presets.begin(), presets.end(), [current = column.widthFrac](double preset) {
      return preset > current + 0.0001;
    });
    column.widthFrac = it == presets.end() ? presets[0] : *it;
    column.savedWidthFrac = 0.0;
    return true;
  }

  bool ScrollingLayout::toggleFullWidth(int columnIndex) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (column.savedWidthFrac > 0.0) {
      column.widthFrac = column.savedWidthFrac;
      column.savedWidthFrac = 0.0;
    } else {
      column.savedWidthFrac = column.widthFrac;
      column.widthFrac = 1.0;
    }
    return column.savedWidthFrac > 0.0;
  }

  bool ScrollingLayout::setWidthFraction(int columnIndex, double fraction) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    column.widthFrac = std::clamp(fraction, 0.15, 1.0);
    column.savedWidthFrac = 0.0;
    return true;
  }

  void ScrollingLayout::clearFullWidthState(int columnIndex) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return;
    }
    m_columns[static_cast<size_t>(columnIndex)].savedWidthFrac = 0.0;
  }

  double ScrollingLayout::widthFraction(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return config().layout.defaultWidthFraction;
    }
    return m_columns[static_cast<size_t>(columnIndex)].widthFrac;
  }

  bool ScrollingLayout::setRowBoundary(int columnIndex, int upperRow, double upperWeight, double lowerWeight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    if (upperRow < 0 || upperRow + 1 >= static_cast<int>(column.views.size())) {
      return false;
    }
    column.heightWeights[static_cast<size_t>(upperRow)] = std::max(kMinHeightWeight, upperWeight);
    column.heightWeights[static_cast<size_t>(upperRow + 1)] = std::max(kMinHeightWeight, lowerWeight);
    return true;
  }

  bool ScrollingLayout::setHeightWeight(int columnIndex, int row, double weight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    if (row < 0 || row >= static_cast<int>(column.heightWeights.size())) {
      return false;
    }
    column.heightWeights[static_cast<size_t>(row)] = std::max(kMinHeightWeight, weight);
    return true;
  }

  bool ScrollingLayout::setTopGapWeight(int columnIndex, double weight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    m_columns[static_cast<size_t>(columnIndex)].topGapWeight = std::max(0.0, weight);
    return true;
  }

  bool ScrollingLayout::setBottomGapWeight(int columnIndex, double weight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    m_columns[static_cast<size_t>(columnIndex)].bottomGapWeight = std::max(0.0, weight);
    return true;
  }

  double ScrollingLayout::heightWeight(int columnIndex, int row) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 1.0;
    }
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (row < 0 || row >= static_cast<int>(column.heightWeights.size())) {
      return 1.0;
    }
    return column.heightWeights[static_cast<size_t>(row)];
  }

  double ScrollingLayout::topGapWeight(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 0.0;
    }
    return m_columns[static_cast<size_t>(columnIndex)].topGapWeight;
  }

  double ScrollingLayout::bottomGapWeight(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 0.0;
    }
    return m_columns[static_cast<size_t>(columnIndex)].bottomGapWeight;
  }

} // namespace umbriel
