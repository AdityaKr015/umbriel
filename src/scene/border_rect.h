#pragma once

#include "view/border_ring.h"

#include <array>

struct wlr_scene_border;

namespace umbriel {

  struct BorderSnapshot {
    wlr_scene_border* node = nullptr;
    std::array<float, 4> innerColor{};
    std::array<float, 4> outerColor{};
  };

  // Position and size the single-pass border relative to the content origin.
  // The render margin belongs only to raster coverage; widths remain logical.
  void applyBorderGeometry(wlr_scene_border* border, const BorderRing& ring, int innerWidth, int outerWidth);

} // namespace umbriel
