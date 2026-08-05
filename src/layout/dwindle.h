#pragma once

#include "layout/layout.h"

#include <cstdint>
#include <memory>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;

  class DwindleLayout : public Layout {
  public:
    struct Node {
      enum Type : uint8_t { Leaf, HSplit, VSplit };
      Type type = Leaf;
      std::unique_ptr<Node> left;
      std::unique_ptr<Node> right;
      Node* parent = nullptr;
      double ratio = 0.5;
      View* view = nullptr;
    };

    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Dwindle; }

    [[nodiscard]] const std::vector<Column>& columns() const override { return m_flatColumns; }
    [[nodiscard]] int columnOf(const View* view) const override;
    [[nodiscard]] int rowOf(const View* view) const override;

    void insertView(View* view, int columnIndex) override;
    void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) override;
    bool consumeLeft(View* view) override;
    bool expelRight(View* view) override;
    bool moveViewVertical(View* view, int direction) override;
    void removeView(View* view) override;
    void moveColumn(int from, int to) override;
    void arrange(const wlr_box& usable) override;

    [[nodiscard]] wlr_box targetBox(const View* view) const override;

    [[nodiscard]] int leafIndexAt(double cx, double cy) const;
    [[nodiscard]] wlr_box targetBoxByIndex(int index) const;
    [[nodiscard]] View* verticalSibling(const View* view, int direction) const;
    [[nodiscard]] View* focusVerticalLeaf(const View* view, int direction) const override;

    bool cycleWidth(int columnIndex) override;
    bool toggleFullWidth(int columnIndex) override;
    bool setWidthFraction(int columnIndex, double fraction) override;
    void clearFullWidthState(int columnIndex) override;
    [[nodiscard]] double widthFraction(int columnIndex) const override;

  private:
    struct Target {
      View* view = nullptr;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
    };

    [[nodiscard]] Node* findNode(const View* view) const;
    [[nodiscard]] Node* nodeAtFlatIndex(int index) const;
    void splitNode(Node* node, View* newView);
    [[nodiscard]] bool isHorizontal(const Node* node) const;
    void arrangeNode(const Node* node, const wlr_box& area);
    void collectColumns(const Node* node);
    void rebuildFlatColumns();
    void detachNode(Node* node);

    std::unique_ptr<Node> m_root;
    mutable std::vector<Column> m_flatColumns;
    mutable std::vector<Target> m_targets;
    int m_splitCounter = 0;
  };

} // namespace umbriel
