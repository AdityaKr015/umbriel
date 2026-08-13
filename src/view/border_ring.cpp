#include "view/border_ring.h"

#include <algorithm>

namespace umbriel {

  std::array<BorderEdge, 4> makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness) {
    const int width = contentWidth + 2 * thickness;
    const int innerWidth = std::max(0, width - 2 * thickness);
    const int sideHeight = std::max(0, contentHeight - 2 * radius);
    const int outer = expandedRadius(radius, thickness);
    return {{
        {
            .box = {-thickness, -thickness, width, thickness + radius},
            .outer = corner_radii_top(outer),
            .hasHole = true,
            .hole = {thickness, thickness, innerWidth, thickness + radius},
            .holeCorners = corner_radii_top(radius),
        },
        {
            .box = {-thickness, contentHeight - radius, width, thickness + radius},
            .outer = corner_radii_bottom(outer),
            .hasHole = true,
            .hole = {thickness, -1, innerWidth, radius + 1},
            .holeCorners = corner_radii_bottom(radius),
        },
        {
            .box = {-thickness, radius, thickness, sideHeight},
            .outer = corner_radii_none(),
            .hasHole = false,
            .hole = {},
            .holeCorners = corner_radii_none(),
        },
        {
            .box = {contentWidth, radius, thickness, sideHeight},
            .outer = corner_radii_none(),
            .hasHole = false,
            .hole = {},
            .holeCorners = corner_radii_none(),
        },
    }};
  }

} // namespace umbriel
