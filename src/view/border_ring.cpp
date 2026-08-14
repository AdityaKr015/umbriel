#include "view/border_ring.h"

namespace umbriel {

  BorderRing makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness) {
    const int outerRadius = expandedRadius(radius, thickness);
    return {
        .box = {-thickness, -thickness, contentWidth + 2 * thickness, contentHeight + 2 * thickness},
        .outer = corner_radii_new(outerRadius, outerRadius, outerRadius, outerRadius),
        .hole = {thickness, thickness, contentWidth, contentHeight},
        .inner = corner_radii_new(radius, radius, radius, radius),
    };
  }

} // namespace umbriel
