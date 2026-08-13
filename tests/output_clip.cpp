#include "view/output_clip.h"

#include <cstdlib>

int main() {
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{-501, 40, 1200, 900};
  const wlr_box visible{0, 40, 700, 900};

  const wlr_box rightEdgeClip = umbriel::surfaceClipForOutput(geometry, content, visible, 0, 0);
  if (rightEdgeClip.x != 501 || rightEdgeClip.width != 700 || rightEdgeClip.x + rightEdgeClip.width != 1201) {
    return EXIT_FAILURE;
  }

  const wlr_box offsetGeometry{4, 7, 1200, 900};
  const wlr_box offsetClip = umbriel::surfaceClipForOutput(offsetGeometry, content, visible, 3, 5);
  if (offsetClip.x != 502
      || offsetClip.y != 2
      || offsetClip.width != visible.width
      || offsetClip.height != visible.height) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
