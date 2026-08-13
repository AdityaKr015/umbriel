#include "view/decoration.h"

#include "config/config.h"
#include "scene/color.h"

// clang-format off
#include <algorithm>
#include "wlr.h"
// clang-format on

namespace umbriel {

  namespace {

    // Which sides of `box` the clip cut away. A cut side must lose its rounding:
    // a radius drawn against the cut would read as the window curving away mid-edge.
    struct Trim {
      bool left;
      bool right;
      bool top;
      bool bottom;

      [[nodiscard]] fx_corner_radii apply(fx_corner_radii corners) const {
        return corner_radii_new(
            left || top ? 0 : corners.top_left, right || top ? 0 : corners.top_right,
            right || bottom ? 0 : corners.bottom_right, left || bottom ? 0 : corners.bottom_left
        );
      }
    };

    [[nodiscard]] Trim trimOf(const wlr_box& visible, const wlr_box& full) {
      return {
          .left = (visible.x > full.x),
          .right = (visible.x + visible.width) < (full.x + full.width),
          .top = (visible.y > full.y),
          .bottom = (visible.y + visible.height) < (full.y + full.height),
      };
    }

    [[nodiscard]] fx_corner_radii uniformRadii(int radius) { return corner_radii_new(radius, radius, radius, radius); }

  } // namespace

  // --- Borders ---

  void ViewDecoration::ensureBorders(wlr_scene_tree* parent) {
    if (m_borderTree != nullptr) {
      return;
    }
    m_borderTree = wlr_scene_tree_create(parent);
    // Outer below, then inner on top so the focus ring stays visible.
    m_outerBorderRect = wlr_scene_rect_create(m_borderTree, 0, 0, config().appearance.outerBorderColor.data());
    for (auto*& rect : m_borderRects) {
      rect = wlr_scene_rect_create(m_borderTree, 0, 0, config().appearance.borderUnfocused.data());
    }
    for (wlr_scene_rect* rect : m_borderRects) {
      wlr_scene_node_raise_to_top(&rect->node);
    }
    wlr_scene_node_lower_to_bottom(&m_borderTree->node);
  }

  bool ViewDecoration::bordersVisible() const { return m_borderTree != nullptr && m_borderTree->node.enabled; }

  void ViewDecoration::setBordersEnabled(bool enabled) {
    if (m_borderTree != nullptr) {
      wlr_scene_node_set_enabled(&m_borderTree->node, enabled);
    }
  }

  void ViewDecoration::updateBorderGeometry(int contentWidth, int contentHeight) {
    if (m_borderTree == nullptr) {
      return;
    }
    const auto edges =
        makeBorderRing(contentWidth, contentHeight, config().appearance.cornerRadius, config().appearance.borderWidth);
    for (size_t i = 0; i < edges.size(); ++i) {
      wlr_scene_rect* rect = m_borderRects[i];
      if (rect == nullptr) {
        continue;
      }
      const BorderEdge& edge = edges[i];
      wlr_scene_node_set_position(&rect->node, edge.box.x, edge.box.y);
      wlr_scene_rect_set_size(rect, edge.box.width, edge.box.height);
      wlr_scene_rect_set_corner_radii(rect, edge.outer);
      wlr_scene_rect_set_clipped_region(
          rect,
          edge.hasHole ? clipped_region{.area = edge.hole, .corners = edge.holeCorners} : clipped_region_get_default()
      );
    }

    if (m_outerBorderRect != nullptr) {
      const int outer = config().appearance.outerBorderWidth;
      const int total = config().appearance.totalBorderWidth();
      const int radius = config().appearance.cornerRadius;
      if (outer <= 0) {
        wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
      } else {
        // Fill the full decoration bounds; hole is only the window surface so the
        // outer color tucks under the inner border (no gap between the two rings).
        wlr_scene_node_set_position(&m_outerBorderRect->node, -total, -total);
        wlr_scene_rect_set_size(m_outerBorderRect, contentWidth + 2 * total, contentHeight + 2 * total);
        wlr_scene_rect_set_corner_radii(m_outerBorderRect, uniformRadii(expandedRadius(radius, total)));
        wlr_scene_rect_set_color(m_outerBorderRect, config().appearance.outerBorderColor.data());
        wlr_scene_rect_set_clipped_region(
            m_outerBorderRect,
            clipped_region{
                .area = {total, total, contentWidth, contentHeight},
                .corners = uniformRadii(radius),
            }
        );
      }
    }

    for (wlr_scene_rect* rect : m_borderRects) {
      if (rect != nullptr) {
        wlr_scene_node_raise_to_top(&rect->node);
      }
    }
  }

  void ViewDecoration::setBorderColor(bool focused, bool scratchpad, float alpha) {
    if (m_borderTree == nullptr) {
      return;
    }
    const auto& baseColor = scratchpad
        ? (focused ? config().appearance.scratchpadBorderFocused : config().appearance.scratchpadBorderUnfocused)
        : (focused ? config().appearance.borderFocused : config().appearance.borderUnfocused);
    float color[4];
    premultiplied(color, baseColor, alpha);
    for (wlr_scene_rect* rect : m_borderRects) {
      if (rect == nullptr) {
        continue;
      }
      wlr_scene_rect_set_color(rect, color);
    }
    if (m_outerBorderRect != nullptr) {
      float outerColor[4];
      premultiplied(outerColor, config().appearance.outerBorderColor, alpha);
      wlr_scene_rect_set_color(m_outerBorderRect, outerColor);
    }
  }

  void
  ViewDecoration::clipBorders(const wlr_box& target, const wlr_box& outputBox, int contentWidth, int contentHeight) {
    if (m_borderTree == nullptr) {
      return;
    }

    // Outer ring first: it is a single rect covering the whole decoration bounds
    // with the surface punched out, so it clips as one box rather than four.
    if (m_outerBorderRect != nullptr) {
      if (config().appearance.outerBorderWidth <= 0) {
        wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
      } else {
        const int total = config().appearance.totalBorderWidth();
        const int radius = config().appearance.cornerRadius;
        const wlr_box screenBox{
            .x = target.x - total,
            .y = target.y - total,
            .width = contentWidth + 2 * total,
            .height = contentHeight + 2 * total,
        };
        wlr_box visible{};
        if (!wlr_box_intersection(&visible, &screenBox, &outputBox)) {
          wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
        } else {
          wlr_scene_node_set_position(&m_outerBorderRect->node, visible.x - target.x, visible.y - target.y);
          wlr_scene_rect_set_size(m_outerBorderRect, visible.width, visible.height);

          const Trim trim = trimOf(visible, screenBox);
          wlr_scene_rect_set_corner_radii(m_outerBorderRect, trim.apply(uniformRadii(expandedRadius(radius, total))));
          // Hole matches the window surface; inner border covers the overlap on top.
          const wlr_box hole{
              .x = screenBox.x + total - visible.x,
              .y = screenBox.y + total - visible.y,
              .width = contentWidth,
              .height = contentHeight,
          };
          wlr_scene_rect_set_clipped_region(
              m_outerBorderRect, clipped_region{.area = hole, .corners = trim.apply(uniformRadii(radius))}
          );
        }
      }
    }

    const auto edges =
        makeBorderRing(contentWidth, contentHeight, config().appearance.cornerRadius, config().appearance.borderWidth);
    for (size_t i = 0; i < edges.size(); ++i) {
      wlr_scene_rect* rect = m_borderRects[i];
      if (rect == nullptr) {
        continue;
      }
      const BorderEdge& edge = edges[i];
      wlr_box screenBox = edge.box;
      screenBox.x += target.x;
      screenBox.y += target.y;

      wlr_box visible{};
      if (!wlr_box_intersection(&visible, &screenBox, &outputBox)) {
        wlr_scene_rect_set_size(rect, 0, 0);
        continue;
      }

      wlr_scene_node_set_position(&rect->node, visible.x - target.x, visible.y - target.y);
      wlr_scene_rect_set_size(rect, visible.width, visible.height);

      const Trim trim = trimOf(visible, screenBox);
      wlr_scene_rect_set_corner_radii(rect, trim.apply(edge.outer));
      if (edge.hasHole) {
        wlr_box hole = edge.hole;
        hole.x += screenBox.x - visible.x;
        hole.y += screenBox.y - visible.y;
        wlr_scene_rect_set_clipped_region(rect, clipped_region{.area = hole, .corners = trim.apply(edge.holeCorners)});
      } else {
        wlr_scene_rect_set_clipped_region(rect, clipped_region_get_default());
      }
    }

    // The inner ring must stay above the outer one after both were resized.
    for (wlr_scene_rect* rect : m_borderRects) {
      if (rect != nullptr) {
        wlr_scene_node_raise_to_top(&rect->node);
      }
    }
  }

  bool ViewDecoration::borderGeometryStale(int contentWidth, int contentHeight) const {
    if (m_borderTree == nullptr) {
      return false;
    }
    const int expectedOuterWidth =
        config().appearance.outerBorderWidth > 0 ? contentWidth + 2 * config().appearance.totalBorderWidth() : 0;
    return m_borderRects[0]->width != contentWidth + 2 * config().appearance.borderWidth
        || m_borderRects[2]->height != std::max(0, contentHeight - 2 * config().appearance.cornerRadius)
        || (m_outerBorderRect != nullptr && m_outerBorderRect->width != expectedOuterWidth);
  }

  void ViewDecoration::snapshotBorders(
      wlr_scene_tree* snapshot, bool focused, std::vector<std::pair<wlr_scene_rect*, std::array<float, 4>>>& out
  ) const {
    if (!bordersVisible()) {
      return;
    }
    const auto& focusedColor = focused ? config().appearance.borderFocused : config().appearance.borderUnfocused;
    const auto copyRect = [&](wlr_scene_rect* src, const std::array<float, 4>& target) {
      wlr_scene_rect* copy = wlr_scene_rect_create(snapshot, src->width, src->height, src->color);
      if (copy == nullptr) {
        return;
      }
      // Snapshot coordinates are relative to the view, so fold in the border
      // tree's own offset: the snapshot tree has no equivalent parent.
      wlr_scene_node_set_position(&copy->node, m_borderTree->node.x + src->node.x, m_borderTree->node.y + src->node.y);
      wlr_scene_rect_set_corner_radii(copy, src->corners);
      wlr_scene_rect_set_clipped_region(copy, src->clipped_region);
      out.emplace_back(copy, target);
    };

    for (wlr_scene_rect* src : m_borderRects) {
      if (src != nullptr) {
        copyRect(src, focusedColor);
      }
    }
    if (m_outerBorderRect != nullptr && config().appearance.outerBorderWidth > 0) {
      copyRect(m_outerBorderRect, config().appearance.outerBorderColor);
    }
  }

  // --- Blur ---

  void ViewDecoration::applyRule(const ResolvedWindowRule& rule) {
    m_blurOptions = SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(rule.blurIgnoreAlpha.value_or(0.0)),
        .enabled = rule.blur.value_or(false),
        .optimized = rule.blurOptimized,
    };
    m_popupBlurOptions = SurfaceBlurOptions{
        .ignoreAlpha = static_cast<float>(rule.blurIgnoreAlpha.value_or(0.0)),
        .enabled = rule.blurPopups.value_or(false),
        .optimized = rule.blurOptimized,
    };
  }

  void ViewDecoration::updateBlur(
      wlr_scene_tree* tree, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& geometry, int radius,
      const wlr_box* clip, float alpha
  ) {
    m_blur.update(tree, surface, nodeBox, geometry, radius, clip, m_blurOptions);
    m_blur.setAlpha(alpha);
  }

  void ViewDecoration::hideBlur() { m_blur.hide(); }

  // --- Shadow ---

  void ViewDecoration::reparentShadow(wlr_scene_tree* layer, int x, int y, bool enabled) {
    if (layer == nullptr) {
      m_shadow.reset();
      if (m_shadowContainer != nullptr) {
        wlr_scene_node_destroy(&m_shadowContainer->node);
        m_shadowContainer = nullptr;
      }
      return;
    }
    if (m_shadowContainer == nullptr) {
      m_shadowContainer = wlr_scene_tree_create(layer);
    } else {
      wlr_scene_node_reparent(&m_shadowContainer->node, layer);
    }
    wlr_scene_node_set_position(&m_shadowContainer->node, x, y);
    wlr_scene_node_set_enabled(&m_shadowContainer->node, enabled);
  }

  void ViewDecoration::setShadowPosition(int x, int y) {
    if (m_shadowContainer != nullptr) {
      wlr_scene_node_set_position(&m_shadowContainer->node, x, y);
    }
  }

  void ViewDecoration::setShadowEnabled(bool enabled) {
    if (m_shadowContainer != nullptr) {
      wlr_scene_node_set_enabled(&m_shadowContainer->node, enabled);
    }
  }

  void ViewDecoration::raiseShadowToTop() {
    if (m_shadowContainer != nullptr) {
      wlr_scene_node_raise_to_top(&m_shadowContainer->node);
    }
  }

  void ViewDecoration::updateShadow(int contentWidth, int contentHeight, int borderInset, int cornerRadius) {
    if (m_shadowContainer == nullptr) {
      return;
    }
    m_shadow.update(m_shadowContainer, contentWidth, contentHeight, borderInset, cornerRadius, shadowClip());
  }

  void ViewDecoration::hideShadow() { m_shadow.hide(); }

  void ViewDecoration::setShadowOutputClip(const wlr_box& clip) {
    if (m_shadowContainer == nullptr) {
      return;
    }
    m_shadowOutputClip = clip;
    m_hasShadowOutputClip = true;
  }

  void ViewDecoration::setAlpha(float alpha) {
    m_shadow.setAlpha(alpha);
    m_blur.setAlpha(alpha);
  }

  void ViewDecoration::hideEffects() {
    m_blur.hide();
    m_shadow.hide();
  }

} // namespace umbriel
