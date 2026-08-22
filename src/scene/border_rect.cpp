#include "scene/border_rect.h"

extern "C" {
#include <scenefx/types/fx/clipped_region.h>
}

namespace umbriel {

  void applyBorderRing(wlr_scene_rect* rect, const BorderRing& ring) {
    if (rect == nullptr) {
      return;
    }

    wlr_scene_node_set_position(&rect->node, ring.box.x, ring.box.y);
    wlr_scene_rect_set_size(rect, ring.box.width, ring.box.height);
    wlr_scene_rect_set_corner_radii(rect, ring.outer);
    wlr_scene_rect_set_clipped_region(rect, clipped_region{.area = ring.hole, .corners = ring.inner});
  }

} // namespace umbriel
