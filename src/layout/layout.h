#pragma once

#include <functional>
#include <memory>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  enum class LayoutMode {
    Scrolling,
    Dwindle,
  };

  struct Column {
    std::vector<View*> views;
    std::vector<double> heightWeights;
    double topGapWeight = 0.0;
    double bottomGapWeight = 0.0;
    double widthFrac = 0.5;
    double savedWidthFrac = 0.0;
  };

  class Layout {
  public:
    virtual ~Layout() = default;

    [[nodiscard]] virtual LayoutMode mode() const = 0;

    [[nodiscard]] virtual int columnOf(const View* view) const = 0;
    [[nodiscard]] virtual int rowOf(const View* view) const = 0;

    virtual void insertView(View* view, int columnIndex) = 0;
    virtual void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) = 0;
    virtual bool consumeLeft(View* view) = 0;
    virtual bool expelRight(View* view) = 0;
    virtual bool moveViewVertical(View* view, int direction) = 0;
    virtual void removeView(View* view) = 0;
    virtual void moveColumn(int from, int to) = 0;
    virtual void arrange(const wlr_box& usable) = 0;

    [[nodiscard]] virtual wlr_box targetBox(const View* view) const = 0;

    [[nodiscard]] virtual View* focusVerticalLeaf(const View* /*view*/, int /*direction*/) const { return nullptr; }

    virtual bool cycleWidth(int columnIndex) = 0;
    virtual bool toggleFullWidth(int columnIndex) = 0;
    virtual bool setWidthFraction(int columnIndex, double fraction) = 0;
    virtual void clearFullWidthState(int columnIndex) = 0;
    [[nodiscard]] virtual double widthFraction(int columnIndex) const = 0;

    // ---- Scrolling-specific (default no-ops for non-scrolling layouts) ----

    [[nodiscard]] virtual const std::vector<Column>& columns() const {
      static const std::vector<Column> empty;
      return empty;
    }

    [[nodiscard]] virtual double scroll() const { return 0; }
    virtual void setScroll(double) {}
    [[nodiscard]] virtual int maxScroll(int) const { return 0; }
    virtual void ensureVisible(int, int) {}
    [[nodiscard]] virtual double scrollAmountToEnsureVisible(int, int) const { return 0; }

    [[nodiscard]] virtual bool isFullWidth(int) const { return false; }
    [[nodiscard]] virtual int columnX(int, int) const { return 0; }
    [[nodiscard]] virtual int columnWidth(int, int) const { return 0; }
    [[nodiscard]] virtual int insertGap() const { return -1; }
    virtual void setInsertGap(int) {}
    virtual void clearInsertGap() {}

    virtual bool setRowBoundary(int, int, double, double) { return false; }
    virtual bool setHeightWeight(int, int, double) { return false; }
    virtual bool setTopGapWeight(int, double) { return false; }
    virtual bool setBottomGapWeight(int, double) { return false; }
    [[nodiscard]] virtual double heightWeight(int, int) const { return 1.0; }
    [[nodiscard]] virtual double topGapWeight(int) const { return 0.0; }
    [[nodiscard]] virtual double bottomGapWeight(int) const { return 0.0; }
  };

  [[nodiscard]] std::unique_ptr<Layout> createLayout(LayoutMode mode);

} // namespace umbriel
