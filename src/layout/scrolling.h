#pragma once

#include "layout/layout.h"

#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  class ScrollingLayout : public Layout {
  public:
    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Scrolling; }

    [[nodiscard]] const std::vector<Column>& columns() const override { return m_columns; }
    [[nodiscard]] int columnOf(const View* view) const override;
    [[nodiscard]] int rowOf(const View* view) const override;
    [[nodiscard]] double scroll() const override { return m_scroll; }
    [[nodiscard]] int columnX(int columnIndex, int viewportWidth) const override;
    [[nodiscard]] int columnWidth(int columnIndex, int viewportWidth) const override;
    [[nodiscard]] bool isFullWidth(int columnIndex) const override;
    [[nodiscard]] int maxScroll(int viewportWidth) const override {
      return std::max(0, totalWidth(viewportWidth) - viewportWidth);
    }

    void insertView(View* view, int columnIndex) override;
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) override;
    bool consumeLeft(View* view) override;
    bool expelRight(View* view) override;
    bool moveViewVertical(View* view, int direction) override;
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    void setScroll(double scroll) override;
    void ensureVisible(int columnIndex, int viewportWidth) override;
    [[nodiscard]] double scrollAmountToEnsureVisible(int columnIndex, int viewportWidth) const override;
    void arrange(const wlr_box& usable) override;
    [[nodiscard]] wlr_box targetBox(const View* view) const override;

    bool cycleWidth(int columnIndex) override;
    bool toggleFullWidth(int columnIndex) override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    [[nodiscard]] double widthFraction(int columnIndex) const override;

    [[nodiscard]] uint32_t resizeEdgesAt(const View* view, double cx, double cy) const override;
    [[nodiscard]] uint32_t sanitizeResizeEdges(const View* view, uint32_t edges) const override;
    std::unique_ptr<ResizeGrab> beginResize(View* view, uint32_t edges, const wlr_box& usable) override;

    bool setRowBoundary(int columnIndex, int upperRow, double upperWeight, double lowerWeight) override;
    bool setHeightWeight(int columnIndex, int row, double weight) override;
    bool setTopGapWeight(int columnIndex, double weight) override;
    bool setBottomGapWeight(int columnIndex, double weight) override;
    [[nodiscard]] double heightWeight(int columnIndex, int row) const override;
    [[nodiscard]] double topGapWeight(int columnIndex) const override;
    [[nodiscard]] double bottomGapWeight(int columnIndex) const override;

  private:
    struct Target {
      View* view = nullptr;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
    };

    [[nodiscard]] int totalWidth(int viewportWidth) const;
    [[nodiscard]] int rawTotalWidth(int viewportWidth) const;
    [[nodiscard]] int centeringOffset(int viewportWidth) const;
    [[nodiscard]] double targetScrollForEnsureVisible(int columnIndex, int viewportWidth) const;
    void syncHeightWeights(Column& column);

    std::vector<Column> m_columns;
    std::vector<Target> m_targets;
    double m_scroll = 0;
  };

} // namespace umbriel
