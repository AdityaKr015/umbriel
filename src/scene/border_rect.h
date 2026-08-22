#pragma once

#include "view/border_ring.h"

struct wlr_scene_rect;

namespace umbriel {

  // Draw `ring` on `rect`, in coordinates relative to the content box origin. The ring is one rounded rectangle with
  // the content punched out of it, so it never sits behind the window: a filled rect would tint every translucent
  // client with the border colour, which would then visibly change with focus. Containment on an output is not this
  // function's business; the caller's scene tree carries the clip.
  void applyBorderRing(wlr_scene_rect* rect, const BorderRing& ring);

} // namespace umbriel
