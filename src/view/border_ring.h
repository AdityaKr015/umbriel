#pragma once

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

namespace umbriel {

  // A rounded border is one outer rectangle with the window punched out of its
  // center. Both curves share a center, so their radii differ by the thickness.
  struct BorderRing {
    wlr_box box;
    fx_corner_radii outer;
    wlr_box hole;
    fx_corner_radii inner;
  };

  // A ring drawn outside a rounded rectangle has to grow its own radius by the
  // ring's thickness to stay concentric. A square window stays square.
  [[nodiscard]] constexpr int expandedRadius(int radius, int thickness) { return radius > 0 ? radius + thickness : 0; }

  [[nodiscard]] BorderRing makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness);

} // namespace umbriel
