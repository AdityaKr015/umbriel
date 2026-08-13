#include "view/output_clip.h"

#include "check.h"

UMBRIEL_TEST(clipsToTheRightEdgeOfTheOutput) {
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{-501, 40, 1200, 900};
  const wlr_box visible{0, 40, 700, 900};

  const wlr_box clip = umbriel::surfaceClipForOutput(geometry, content, visible, 0, 0);

  CHECK_EQ(clip.x, 501);
  CHECK_EQ(clip.width, 700);
  CHECK_EQ(clip.x + clip.width, 1201);
}

UMBRIEL_TEST(offsetGeometryShiftsTheClipOrigin) {
  const wlr_box geometry{4, 7, 1200, 900};
  const wlr_box content{-501, 40, 1200, 900};
  const wlr_box visible{0, 40, 700, 900};

  const wlr_box clip = umbriel::surfaceClipForOutput(geometry, content, visible, 3, 5);

  CHECK_EQ(clip.x, 502);
  CHECK_EQ(clip.y, 2);
  CHECK_EQ(clip.width, visible.width);
  CHECK_EQ(clip.height, visible.height);
}

int main() { return RUN_TESTS(); }
