#pragma once

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

namespace umbriel {

  // Raster bounds and the content hole for a single-pass border.
  struct BorderRing {
    wlr_box box;
    wlr_box hole;
    fx_corner_radii inner;
  };

  // Expands a rounded radius while preserving square corners.
  [[nodiscard]] constexpr int expandedRadius(int radius, int thickness) { return radius > 0 ? radius + thickness : 0; }

  [[nodiscard]] BorderRing makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness);

} // namespace umbriel
