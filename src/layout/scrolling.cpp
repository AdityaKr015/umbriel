#include "layout/scrolling.h"

#include "config/config.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on

namespace umbriel {

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
    int x = 0;
    for (int i = 0; i < end; ++i) {
      x += columnWidth(i, viewportWidth) + config().layout.gap;
    }
    if (m_insertGap >= 0 && end >= m_insertGap) {
      x += kHintWidth + config().layout.gap;
    }
    return x;
  }

  int ScrollingLayout::totalWidth(int viewportWidth) const {
    if (m_columns.empty()) {
      return 0;
    }
    return columnX(static_cast<int>(m_columns.size()), viewportWidth) - config().layout.gap;
  }

  void ScrollingLayout::insertView(View* view, int columnIndex) {
    if (view == nullptr || columnOf(view) >= 0) {
      return;
    }
    const int index = std::clamp(columnIndex, 0, static_cast<int>(m_columns.size()));
    Column column;
    column.views.push_back(view);
    m_columns.insert(m_columns.begin() + index, std::move(column));
  }

  void ScrollingLayout::insertViewIntoColumn(View* view, int columnIndex, int rowIndex) {
    if (view == nullptr
        || columnOf(view) >= 0
        || columnIndex < 0
        || columnIndex >= static_cast<int>(m_columns.size())) {
      return;
    }
    auto& views = m_columns[static_cast<size_t>(columnIndex)].views;
    const int row = std::clamp(rowIndex, 0, static_cast<int>(views.size()));
    views.insert(views.begin() + row, view);
  }

  bool ScrollingLayout::consumeLeft(View* view) {
    const int sourceColumn = columnOf(view);
    if (sourceColumn <= 0) {
      return false;
    }
    auto& sourceViews = m_columns[static_cast<size_t>(sourceColumn)].views;
    std::erase(sourceViews, view);
    m_columns[static_cast<size_t>(sourceColumn - 1)].views.push_back(view);
    if (sourceViews.empty()) {
      m_columns.erase(m_columns.begin() + sourceColumn);
    }
    return true;
  }

  bool ScrollingLayout::expelRight(View* view) {
    const int sourceColumn = columnOf(view);
    if (sourceColumn < 0) {
      return false;
    }
    auto& sourceViews = m_columns[static_cast<size_t>(sourceColumn)].views;
    if (sourceViews.size() <= 1) {
      return false;
    }
    std::erase(sourceViews, view);
    Column column;
    column.views.push_back(view);
    m_columns.insert(m_columns.begin() + sourceColumn + 1, std::move(column));
    return true;
  }

  bool ScrollingLayout::moveViewVertical(View* view, int direction) {
    const int column = columnOf(view);
    const int row = rowOf(view);
    if (column < 0 || row < 0) {
      return false;
    }
    auto& views = m_columns[static_cast<size_t>(column)].views;
    const int target = row + direction;
    if (target < 0 || target >= static_cast<int>(views.size())) {
      return false;
    }
    std::swap(views[static_cast<size_t>(row)], views[static_cast<size_t>(target)]);
    return true;
  }

  void ScrollingLayout::removeView(View* view) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return;
    }
    auto& views = m_columns[static_cast<size_t>(columnIndex)].views;
    std::erase(views, view);
    if (views.empty()) {
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

  void ScrollingLayout::setScroll(double scroll) { m_scroll = std::max(0.0, scroll); }

  void ScrollingLayout::ensureVisible(int columnIndex, int viewportWidth) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size()) || viewportWidth <= 0) {
      return;
    }
    const int x = columnX(columnIndex, viewportWidth);
    const int width = columnWidth(columnIndex, viewportWidth);
    m_scroll = std::max(std::min(m_scroll, static_cast<double>(x)), static_cast<double>(x + width - viewportWidth));
    m_scroll = std::clamp(m_scroll, 0.0, static_cast<double>(std::max(0, totalWidth(viewportWidth) - viewportWidth)));
  }

  void ScrollingLayout::arrange(const wlr_box& usable) {
    m_targets.clear();
    const int viewportWidth = std::max(1, usable.width - 2 * config().layout.gap);
    const int availableHeight = std::max(1, usable.height - 2 * config().layout.gap);
    m_scroll = std::clamp(m_scroll, 0.0, static_cast<double>(std::max(0, totalWidth(viewportWidth) - viewportWidth)));

    for (size_t columnIndex = 0; columnIndex < m_columns.size(); ++columnIndex) {
      const Column& column = m_columns[columnIndex];
      if (column.views.empty()) {
        continue;
      }
      const int width = columnWidth(static_cast<int>(columnIndex), viewportWidth);
      const int x = usable.x
          + config().layout.gap
          + columnX(static_cast<int>(columnIndex), viewportWidth)
          - static_cast<int>(std::lround(m_scroll));
      const int rowCount = static_cast<int>(column.views.size());
      const int rowsHeight = std::max(rowCount, availableHeight - (rowCount - 1) * config().layout.gap);
      int y = usable.y + config().layout.gap;
      int remainingHeight = rowsHeight;
      for (int row = 0; row < rowCount; ++row) {
        const int remainingRows = rowCount - row;
        const int height = std::max(1, remainingHeight / remainingRows);
        m_targets.push_back(
            {.view = column.views[static_cast<size_t>(row)], .x = x, .y = y, .width = width, .height = height}
        );
        y += height + config().layout.gap;
        remainingHeight -= height;
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

  double ScrollingLayout::widthFraction(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return config().layout.defaultWidthFraction;
    }
    return m_columns[static_cast<size_t>(columnIndex)].widthFrac;
  }

} // namespace umbriel
