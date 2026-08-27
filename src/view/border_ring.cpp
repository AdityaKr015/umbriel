#include "view/border_ring.h"

namespace umbriel {

  BorderRing makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness) {
    // One transparent logical pixel gives fractional outer coverage a fragment
    // to land in instead of clipping it at the scene box.
    const int renderMargin = thickness > 0 ? 1 : 0;
    const int extent = thickness + renderMargin;
    return {
        .box = {-extent, -extent, contentWidth + 2 * extent, contentHeight + 2 * extent},
        .hole = {extent, extent, contentWidth, contentHeight},
        .inner = corner_radii_new(radius, radius, radius, radius),
    };
  }

} // namespace umbriel
