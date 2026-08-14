#include "scene/border_rect.h"

#include "view/output_clip.h"

namespace umbriel {

  bool applyBorderRing(wlr_scene_rect* rect, const BorderRing& ring, int contentX, int contentY, const wlr_box* clip) {
    if (rect == nullptr) {
      return false;
    }

    wlr_box full = ring.box;
    full.x += contentX;
    full.y += contentY;

    wlr_box visible = full;
    if (clip != nullptr && !wlr_box_intersection(&visible, &full, clip)) {
      wlr_scene_rect_set_size(rect, 0, 0);
      return false;
    }

    wlr_scene_node_set_position(&rect->node, visible.x - contentX, visible.y - contentY);
    wlr_scene_rect_set_size(rect, visible.width, visible.height);
    wlr_scene_rect_set_corner_radii(rect, cornerRadiiForVisible(full, visible, ring.outer));

    wlr_box hole = ring.hole;
    hole.x += full.x - visible.x;
    hole.y += full.y - visible.y;
    wlr_scene_rect_set_clipped_region(
        rect, clipped_region{.area = hole, .corners = cornerRadiiForVisible(full, visible, ring.inner)}
    );
    return true;
  }

} // namespace umbriel
