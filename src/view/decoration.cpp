#include "view/decoration.h"

#include "config/config.h"
#include "scene/color.h"
#include "view/output_clip.h"

// clang-format off
#include "wlr.h"
// clang-format on

namespace umbriel {

  // --- Borders ---

  void ViewDecoration::ensureBorders(wlr_scene_tree* parent) {
    if (m_borderTree != nullptr) {
      return;
    }
    m_borderTree = wlr_scene_tree_create(parent);
    // Outer below, then inner on top so the focus ring stays visible.
    m_outerBorderRect = wlr_scene_rect_create(m_borderTree, 0, 0, config().appearance.outerBorderColor.data());
    m_borderRect = wlr_scene_rect_create(m_borderTree, 0, 0, config().appearance.borderUnfocused.data());
    wlr_scene_node_raise_to_top(&m_borderRect->node);
    // The punched hole protects client content, so keep the ring above the
    // toplevel surface that would otherwise cover its outermost pixels.
    wlr_scene_node_raise_to_top(&m_borderTree->node);
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

    const auto applyRing = [](wlr_scene_rect* rect, const BorderRing& ring) {
      wlr_scene_node_set_position(&rect->node, ring.box.x, ring.box.y);
      wlr_scene_rect_set_size(rect, ring.box.width, ring.box.height);
      wlr_scene_rect_set_corner_radii(rect, ring.outer);
      wlr_scene_rect_set_clipped_region(rect, clipped_region{.area = ring.hole, .corners = ring.inner});
    };

    if (m_borderRect != nullptr) {
      applyRing(
          m_borderRect,
          makeBorderRing(contentWidth, contentHeight, config().appearance.cornerRadius, config().appearance.borderWidth)
      );
    }

    if (m_outerBorderRect != nullptr) {
      const int outer = config().appearance.outerBorderWidth;
      if (outer <= 0) {
        wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
      } else {
        // The outer color tucks under the inner ring, leaving no gap between them.
        applyRing(
            m_outerBorderRect,
            makeBorderRing(
                contentWidth, contentHeight, config().appearance.cornerRadius, config().appearance.totalBorderWidth()
            )
        );
        wlr_scene_rect_set_color(m_outerBorderRect, config().appearance.outerBorderColor.data());
      }
    }

    if (m_borderRect != nullptr) {
      wlr_scene_node_raise_to_top(&m_borderRect->node);
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
    if (m_borderRect != nullptr) {
      wlr_scene_rect_set_color(m_borderRect, color);
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

    const auto clipRing = [&](wlr_scene_rect* rect, int thickness) {
      if (rect == nullptr) {
        return;
      }
      if (thickness <= 0) {
        wlr_scene_rect_set_size(rect, 0, 0);
        return;
      }

      const BorderRing ring = makeBorderRing(contentWidth, contentHeight, config().appearance.cornerRadius, thickness);
      wlr_box screenBox = ring.box;
      screenBox.x += target.x;
      screenBox.y += target.y;

      wlr_box visible{};
      if (!wlr_box_intersection(&visible, &screenBox, &outputBox)) {
        wlr_scene_rect_set_size(rect, 0, 0);
        return;
      }

      wlr_scene_node_set_position(&rect->node, visible.x - target.x, visible.y - target.y);
      wlr_scene_rect_set_size(rect, visible.width, visible.height);

      wlr_scene_rect_set_corner_radii(rect, cornerRadiiForVisible(screenBox, visible, ring.outer));
      wlr_box hole = ring.hole;
      hole.x += screenBox.x - visible.x;
      hole.y += screenBox.y - visible.y;
      wlr_scene_rect_set_clipped_region(
          rect, clipped_region{.area = hole, .corners = cornerRadiiForVisible(screenBox, visible, ring.inner)}
      );
    };

    if (config().appearance.outerBorderWidth > 0) {
      clipRing(m_outerBorderRect, config().appearance.totalBorderWidth());
    } else if (m_outerBorderRect != nullptr) {
      wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
    }
    clipRing(m_borderRect, config().appearance.borderWidth);

    if (m_borderRect != nullptr) {
      wlr_scene_node_raise_to_top(&m_borderRect->node);
    }
  }

  bool ViewDecoration::borderGeometryStale(int contentWidth, int contentHeight) const {
    if (m_borderTree == nullptr) {
      return false;
    }
    const int inner = config().appearance.borderWidth;
    const int expectedOuterWidth =
        config().appearance.outerBorderWidth > 0 ? contentWidth + 2 * config().appearance.totalBorderWidth() : 0;
    return (m_borderRect != nullptr
            && (m_borderRect->width != contentWidth + 2 * inner || m_borderRect->height != contentHeight + 2 * inner))
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

    if (m_borderRect != nullptr) {
      copyRect(m_borderRect, focusedColor);
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
