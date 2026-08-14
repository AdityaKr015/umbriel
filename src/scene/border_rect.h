#pragma once

#include "view/border_ring.h"

struct wlr_scene_rect;

namespace umbriel {

  // Draw `ring` on `rect`. The node's coordinates are relative to the content box
  // origin, which sits at (contentX, contentY) in the same space as `clip`.
  //
  // The ring is one rounded rectangle with the content punched out of it, so it
  // never sits behind the window: a filled rect would tint every translucent
  // client with the border colour, which would then visibly change with focus.
  //
  // With a `clip` the rect shrinks to its visible part, the hole shifts to stay
  // aligned with the content, and every corner the clip cuts loses its radius so
  // the new edge reads as a clip and not as a smaller rounded window. Returns
  // false when nothing is visible, leaving the rect sized 0x0.
  bool applyBorderRing(wlr_scene_rect* rect, const BorderRing& ring, int contentX, int contentY, const wlr_box* clip);

} // namespace umbriel
