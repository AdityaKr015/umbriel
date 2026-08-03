#pragma once

#include "config/config.h"

#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  inline constexpr int kHintWidth = 300;

  struct Column {
    std::vector<View*> views;
    // Parallel to views; equal weights share height evenly.
    std::vector<double> heightWeights;
    // Empty space above/below the stack so top/bottom edges can move independently.
    double topGapWeight = 0.0;
    double bottomGapWeight = 0.0;
    double widthFrac = config().layout.defaultWidthFraction;
    double savedWidthFrac = 0.0;
  };

  class ScrollingLayout {
  public:
    [[nodiscard]] const std::vector<Column>& columns() const { return m_columns; }
    [[nodiscard]] int columnOf(const View* view) const;
    [[nodiscard]] int rowOf(const View* view) const;
    [[nodiscard]] double scroll() const { return m_scroll; }
    [[nodiscard]] int insertGap() const { return m_insertGap; }
    [[nodiscard]] int columnX(int columnIndex, int viewportWidth) const;
    [[nodiscard]] int columnWidth(int columnIndex, int viewportWidth) const;
    [[nodiscard]] bool isFullWidth(int columnIndex) const;

    void insertView(View* view, int columnIndex);
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex);
    bool consumeLeft(View* view);
    bool expelRight(View* view);
    bool moveViewVertical(View* view, int direction);
    void removeView(View* view);
    void moveColumn(int from, int to);
    void setScroll(double scroll);
    void setInsertGap(int gapIndex);
    void clearInsertGap();
    void ensureVisible(int columnIndex, int viewportWidth);
    // Fraction of viewport width that ensureVisible would scroll.
    [[nodiscard]] double scrollAmountToEnsureVisible(int columnIndex, int viewportWidth) const;
    void arrange(const wlr_box& usable);
    [[nodiscard]] wlr_box targetBox(const View* view) const;

    bool cycleWidth(int columnIndex);
    bool toggleFullWidth(int columnIndex);
    bool setWidthFraction(int columnIndex, double fraction);
    void clearFullWidthState(int columnIndex);
    [[nodiscard]] double widthFraction(int columnIndex) const;

    // Vertical resize: change the shared boundary between upperRow and upperRow+1,
    // or the top/bottom gap when upperRow is -1 / last.
    bool setRowBoundary(int columnIndex, int upperRow, double upperWeight, double lowerWeight);
    bool setHeightWeight(int columnIndex, int row, double weight);
    bool setTopGapWeight(int columnIndex, double weight);
    bool setBottomGapWeight(int columnIndex, double weight);
    [[nodiscard]] double heightWeight(int columnIndex, int row) const;
    [[nodiscard]] double topGapWeight(int columnIndex) const;
    [[nodiscard]] double bottomGapWeight(int columnIndex) const;

  private:
    struct Target {
      View* view = nullptr;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
    };

    [[nodiscard]] int totalWidth(int viewportWidth) const;
    [[nodiscard]] double targetScrollForEnsureVisible(int columnIndex, int viewportWidth) const;
    void syncHeightWeights(Column& column);

    std::vector<Column> m_columns;
    std::vector<Target> m_targets;
    double m_scroll = 0;
    int m_insertGap = -1;
  };

} // namespace umbriel
