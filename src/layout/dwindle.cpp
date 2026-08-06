#include "layout/dwindle.h"

#include "config/config.h"
#include "view/view.h"
#include "view/xdg_size.h"

// clang-format off
#include <algorithm>
#include <cmath>
#include <ranges>
#include "wlr.h"
// clang-format on

namespace umbriel {

  DwindleLayout::Node* DwindleLayout::findNode(const View* view) const {
    if (m_root == nullptr) {
      return nullptr;
    }
    std::vector<Node*> stack;
    stack.push_back(m_root.get());
    while (!stack.empty()) {
      Node* node = stack.back();
      stack.pop_back();
      if (node->type == Node::Leaf) {
        if (node->view == view) {
          return node;
        }
      } else {
        if (node->right != nullptr) {
          stack.push_back(node->right.get());
        }
        if (node->left != nullptr) {
          stack.push_back(node->left.get());
        }
      }
    }
    return nullptr;
  }

  DwindleLayout::Node* DwindleLayout::nodeAtFlatIndex(int index) const {
    if (m_root == nullptr || index < 0) {
      return nullptr;
    }
    int count = 0;
    std::vector<Node*> stack;
    stack.push_back(m_root.get());
    Node* node = nullptr;
    while (!stack.empty()) {
      node = stack.back();
      stack.pop_back();
      if (node->type == Node::Leaf) {
        if (count == index) {
          return node;
        }
        ++count;
      } else {
        if (node->right != nullptr) {
          stack.push_back(node->right.get());
        }
        if (node->left != nullptr) {
          stack.push_back(node->left.get());
        }
      }
    }
    return nullptr;
  }

  bool DwindleLayout::isHorizontal(const Node* node) const {
    if (node == nullptr) {
      return false;
    }
    int depth = 0;
    Node* current = node->parent;
    while (current != nullptr) {
      ++depth;
      current = current->parent;
    }
    return (depth % 2) == 0;
  }

  void DwindleLayout::splitNode(Node* node, View* newView) {
    if (node == nullptr) {
      return;
    }
    View* oldView = node->view;
    node->view = nullptr;
    const bool horizontal = isHorizontal(node);
    node->type = horizontal ? Node::HSplit : Node::VSplit;
    node->ratio = 0.5;

    auto left = std::make_unique<Node>();
    left->type = Node::Leaf;
    left->view = oldView;
    left->parent = node;

    auto right = std::make_unique<Node>();
    right->type = Node::Leaf;
    right->view = newView;
    right->parent = node;

    node->left = std::move(left);
    node->right = std::move(right);
    ++m_splitCounter;
  }

  void DwindleLayout::detachNode(Node* node) {
    if (node == nullptr) {
      return;
    }
    Node* parent = node->parent;
    if (parent == nullptr) {
      m_root.reset();
      return;
    }
    Node* grandparent = parent->parent;
    Node* sibling = (parent->left.get() == node) ? parent->right.get() : parent->left.get();
    if (sibling == nullptr) {
      if (grandparent != nullptr) {
        if (grandparent->left.get() == parent) {
          grandparent->left.reset();
        } else {
          grandparent->right.reset();
        }
      } else {
        m_root.reset();
      }
      return;
    }
    sibling->parent = nullptr;
    std::unique_ptr<Node> ownedSibling;
    if (parent->left.get() == node) {
      ownedSibling = std::move(parent->right);
    } else {
      ownedSibling = std::move(parent->left);
    }
    parent->left.reset();
    parent->right.reset();
    ownedSibling->parent = grandparent;
    if (grandparent != nullptr) {
      if (grandparent->left.get() == parent) {
        grandparent->left = std::move(ownedSibling);
      } else {
        grandparent->right = std::move(ownedSibling);
      }
    } else {
      m_root = std::move(ownedSibling);
    }
  }

  void DwindleLayout::arrangeNode(const Node* node, const wlr_box& area) {
    if (node == nullptr || area.width <= 0 || area.height <= 0) {
      return;
    }
    if (node->type == Node::Leaf) {
      if (node->view != nullptr) {
        m_targets.push_back({
            .view = node->view,
            .x = area.x,
            .y = area.y,
            .width = area.width,
            .height = area.height,
        });
      }
      return;
    }

    const int gap = m_config->totalGap;
    const double ratio = std::clamp(node->ratio, 0.1, 0.9);

    if (isHorizontal(node)) {
      const int totalWidth = area.width;
      const int leftWidth = std::max(1, static_cast<int>(std::lround(ratio * totalWidth)) - gap / 2);
      const int rightWidth = std::max(1, totalWidth - leftWidth - gap);

      wlr_box leftArea = area;
      leftArea.width = leftWidth;
      wlr_box rightArea = area;
      rightArea.x = area.x + leftWidth + gap;
      rightArea.width = rightWidth;

      if (node->left != nullptr) {
        arrangeNode(node->left.get(), leftArea);
      }
      if (node->right != nullptr) {
        arrangeNode(node->right.get(), rightArea);
      }
    } else {
      const int totalHeight = area.height;
      const int topHeight = std::max(1, static_cast<int>(std::lround(ratio * totalHeight)) - gap / 2);
      const int bottomHeight = std::max(1, totalHeight - topHeight - gap);

      wlr_box topArea = area;
      topArea.height = topHeight;
      wlr_box bottomArea = area;
      bottomArea.y = area.y + topHeight + gap;
      bottomArea.height = bottomHeight;

      if (node->left != nullptr) {
        arrangeNode(node->left.get(), topArea);
      }
      if (node->right != nullptr) {
        arrangeNode(node->right.get(), bottomArea);
      }
    }
  }

  void DwindleLayout::collectColumns(const Node* node) {
    if (node == nullptr) {
      return;
    }
    if (node->type == Node::Leaf) {
      if (node->view != nullptr) {
        Column col;
        col.views.push_back(node->view);
        col.heightWeights.push_back(1.0);
        col.widthFrac = 0.5;
        m_flatColumns.push_back(std::move(col));
      }
      return;
    }
    if (node->left != nullptr) {
      collectColumns(node->left.get());
    }
    if (node->right != nullptr) {
      collectColumns(node->right.get());
    }
  }

  void DwindleLayout::rebuildFlatColumns() {
    m_flatColumns.clear();
    collectColumns(m_root.get());
  }

  // ---- Public Layout interface ----

  int DwindleLayout::columnOf(const View* view) const {
    for (size_t i = 0; i < m_flatColumns.size(); ++i) {
      if (!m_flatColumns[i].views.empty() && m_flatColumns[i].views[0] == view) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int DwindleLayout::rowOf(const View* /*view*/) const { return 0; }

  void DwindleLayout::insertView(View* view, int columnIndex) {
    if (view == nullptr || columnOf(view) >= 0) {
      return;
    }
    if (m_root == nullptr) {
      auto node = std::make_unique<Node>();
      node->type = Node::Leaf;
      node->view = view;
      m_root = std::move(node);
      return;
    }
    const int count = static_cast<int>(m_flatColumns.size());
    const int targetIndex = std::clamp(columnIndex, 0, count);
    Node* target = nullptr;
    if (targetIndex >= count) {
      target = nodeAtFlatIndex(count - 1);
    } else {
      target = nodeAtFlatIndex(targetIndex);
    }
    if (target != nullptr && target->type == Node::Leaf) {
      splitNode(target, view);
    } else if (targetIndex < count) {
      target = nodeAtFlatIndex(targetIndex);
      if (target != nullptr && target->type == Node::Leaf) {
        splitNode(target, view);
      }
    }
  }

  void DwindleLayout::insertViewIntoColumn(View* view, int columnIndex, int /*rowIndex*/) {
    insertView(view, columnIndex);
  }

  bool DwindleLayout::consumeLeft(View* view) {
    Node* a = findNode(view);
    const int col = columnOf(view);
    if (a == nullptr || col <= 0) {
      return false;
    }
    Node* b = nodeAtFlatIndex(col - 1);
    if (b == nullptr || b->type != Node::Leaf) {
      return false;
    }
    std::swap(a->view, b->view);
    return true;
  }

  bool DwindleLayout::expelRight(View* view) {
    Node* a = findNode(view);
    const int col = columnOf(view);
    if (a == nullptr || col < 0 || col + 1 >= static_cast<int>(m_flatColumns.size())) {
      return false;
    }
    Node* b = nodeAtFlatIndex(col + 1);
    if (b == nullptr || b->type != Node::Leaf) {
      return false;
    }
    std::swap(a->view, b->view);
    return true;
  }

  bool DwindleLayout::moveViewVertical(View* view, int direction) {
    Node* a = findNode(view);
    if (a == nullptr || a->parent == nullptr) {
      return false;
    }
    if (a->parent->type != Node::VSplit) {
      return false;
    }
    Node* sibling = nullptr;
    if (direction < 0 && a->parent->right.get() == a) {
      sibling = a->parent->left.get();
    } else if (direction > 0 && a->parent->left.get() == a) {
      sibling = a->parent->right.get();
    }
    if (sibling == nullptr || sibling->type != Node::Leaf) {
      return false;
    }
    std::swap(a->view, sibling->view);
    return true;
  }

  void DwindleLayout::removeView(View* view) {
    Node* node = findNode(view);
    if (node == nullptr) {
      return;
    }
    detachNode(node);
    std::erase_if(m_targets, [view](const Target& t) { return t.view == view; });
  }

  void DwindleLayout::moveColumn(int from, int to) {
    const int count = static_cast<int>(m_flatColumns.size());
    if (from < 0 || from >= count) {
      return;
    }
    const int destination = std::clamp(to, 0, count - 1);
    if (from == destination) {
      return;
    }
    Node* a = nodeAtFlatIndex(from);
    Node* b = nodeAtFlatIndex(destination);
    if (a != nullptr && b != nullptr && a->type == Node::Leaf && b->type == Node::Leaf) {
      std::swap(a->view, b->view);
    }
  }

  void DwindleLayout::arrange(const wlr_box& usable) {
    m_targets.clear();
    const int edgePad = m_config->edgePad;
    wlr_box area{
        .x = usable.x + edgePad,
        .y = usable.y + edgePad,
        .width = std::max(1, usable.width - 2 * edgePad),
        .height = std::max(1, usable.height - 2 * edgePad),
    };
    if (m_root != nullptr) {
      arrangeNode(m_root.get(), area);
    }
    rebuildFlatColumns();
  }

  wlr_box DwindleLayout::targetBox(const View* view) const {
    const auto it = std::ranges::find_if(m_targets, [view](const Target& t) { return t.view == view; });
    if (it == m_targets.end()) {
      return {};
    }
    return {.x = it->x, .y = it->y, .width = it->width, .height = it->height};
  }

  int DwindleLayout::leafIndexAt(double cx, double cy) const {
    for (int i = 0; i < static_cast<int>(m_targets.size()); ++i) {
      const Target& t = m_targets[i];
      if (cx >= t.x && cx < t.x + t.width && cy >= t.y && cy < t.y + t.height) {
        return i;
      }
    }
    return -1;
  }

  wlr_box DwindleLayout::targetBoxByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(m_targets.size())) {
      return {};
    }
    const Target& t = m_targets[static_cast<size_t>(index)];
    return {.x = t.x, .y = t.y, .width = t.width, .height = t.height};
  }

  View* DwindleLayout::verticalSibling(const View* view, int direction) const {
    Node* node = findNode(view);
    if (node == nullptr || node->parent == nullptr) {
      return nullptr;
    }
    if (node->parent->type != Node::VSplit) {
      return nullptr;
    }
    Node* sibling = nullptr;
    if (direction < 0 && node->parent->right.get() == node) {
      sibling = node->parent->left.get();
    } else if (direction > 0 && node->parent->left.get() == node) {
      sibling = node->parent->right.get();
    }
    if (sibling == nullptr || sibling->type != Node::Leaf) {
      return nullptr;
    }
    return sibling->view;
  }

  View* DwindleLayout::focusVerticalLeaf(const View* view, int direction) const {
    return verticalSibling(view, direction);
  }

  bool DwindleLayout::cycleWidth(int columnIndex) {
    Node* node = nodeAtFlatIndex(columnIndex);
    if (node == nullptr || node->parent == nullptr) {
      return false;
    }
    const auto& presets = m_config->widthPresets;
    const auto it = std::ranges::find_if(presets, [current = node->parent->ratio](double preset) {
      return preset > current + 0.0001;
    });
    node->parent->ratio = (it == presets.end()) ? presets[0] : *it;
    return true;
  }

  bool DwindleLayout::toggleFullWidth(int columnIndex) {
    Node* node = nodeAtFlatIndex(columnIndex);
    if (node == nullptr || node->parent == nullptr) {
      return false;
    }
    Node* parent = node->parent;
    if (parent->ratio >= 0.99) {
      parent->ratio = 0.5;
      return false;
    }
    parent->ratio = 1.0;
    return true;
  }

  bool DwindleLayout::setWidthFraction(int columnIndex, double fraction) {
    Node* node = nodeAtFlatIndex(columnIndex);
    if (node == nullptr || node->parent == nullptr) {
      return false;
    }
    node->parent->ratio = std::clamp(fraction, 0.15, 1.0);
    return true;
  }

  void DwindleLayout::clearFullWidthState(int /*columnIndex*/) {}

  double DwindleLayout::widthFraction(int columnIndex) const {
    Node* node = nodeAtFlatIndex(columnIndex);
    if (node != nullptr && node->parent != nullptr) {
      return node->parent->ratio;
    }
    return m_config->defaultWidthFraction;
  }

} // namespace umbriel
