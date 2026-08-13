#pragma once

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
#include "wlr.h"
// clang-format on

#include <array>

namespace umbriel {

  // One side of the border ring, in coordinates relative to the window surface's
  // top-left corner. The top and bottom edges are drawn as full-width rounded
  // slabs with a hole punched where the surface shows through, which is what
  // gives the corners their radius without four separate corner nodes; the left
  // and right edges are plain rectangles spanning the straight part in between.
  struct BorderEdge {
    wlr_box box;
    fx_corner_radii outer;
    bool hasHole;
    wlr_box hole;
    fx_corner_radii holeCorners;
  };

  // A ring drawn outside a rounded rectangle has to grow its own radius by the
  // ring's thickness to stay concentric. A square window stays square.
  [[nodiscard]] constexpr int expandedRadius(int radius, int thickness) { return radius > 0 ? radius + thickness : 0; }

  // The four edges enclosing a `contentWidth` x `contentHeight` surface.
  [[nodiscard]] std::array<BorderEdge, 4>
  makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness);

} // namespace umbriel
