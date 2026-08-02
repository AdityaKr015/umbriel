#pragma once

#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  inline constexpr int kGap = 8;
  inline constexpr double kWidthPresets[] = {1.0 / 3, 0.5, 2.0 / 3};
  inline constexpr double kDefaultWidthFrac = 0.5;
  inline constexpr int kScrollWheelStep = 60;

  struct Column {
    std::vector<View*> views;
    double widthFrac = kDefaultWidthFrac;
    double savedWidthFrac = 0.0;
  };

  class ScrollingLayout {
  public:
    [[nodiscard]] const std::vector<Column>& columns() const { return m_columns; }
    [[nodiscard]] int columnOf(const View* view) const;
    [[nodiscard]] int rowOf(const View* view) const;
    [[nodiscard]] double scroll() const { return m_scroll; }
    [[nodiscard]] int columnX(int columnIndex, int viewportWidth) const;
    [[nodiscard]] int columnWidth(int columnIndex, int viewportWidth) const;

    void insertView(View* view, int columnIndex);
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex);
    bool consumeLeft(View* view);
    bool expelRight(View* view);
    bool moveViewVertical(View* view, int direction);
    void removeView(View* view);
    void moveColumn(int from, int to);
    void setScroll(double scroll);
    void ensureVisible(int columnIndex, int viewportWidth);
    void arrange(const wlr_box& usable);
    [[nodiscard]] wlr_box targetBox(const View* view) const;

    bool cycleWidth(int columnIndex);
    bool toggleFullWidth(int columnIndex);
    bool setWidthFraction(int columnIndex, double fraction);
    [[nodiscard]] double widthFraction(int columnIndex) const;

  private:
    struct Target {
      View* view = nullptr;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
    };

    [[nodiscard]] int totalWidth(int viewportWidth) const;

    std::vector<Column> m_columns;
    std::vector<Target> m_targets;
    double m_scroll = 0;
  };

} // namespace umbriel
