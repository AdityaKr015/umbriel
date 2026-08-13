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

UMBRIEL_TEST(blurClipStaysInsideTheNodeAndTheOutput) {
  // Node fully inside a large output: nothing to trim, nothing to inset.
  const wlr_box nodeBox{0, 0, 800, 600};
  const wlr_box target{100, 100, 800, 600};
  const wlr_box contentVisible{100, 100, 800, 600};
  const wlr_box outputBox{0, 0, 1920, 1080};

  const wlr_box clip = umbriel::blurClipForOutput(nodeBox, contentVisible, outputBox, target, 0);

  CHECK_EQ(clip.x, 0);
  CHECK_EQ(clip.y, 0);
  CHECK_EQ(clip.width, 800);
  CHECK_EQ(clip.height, 600);
}

UMBRIEL_TEST(blurInsetsOnlyTheEdgesTouchingTheOutput) {
  // A view flush against the output's left edge. Blur sampling would reach onto
  // the neighbouring output there, so only that edge is pulled in.
  const wlr_box nodeBox{0, 0, 800, 600};
  const wlr_box target{0, 100, 800, 600};
  const wlr_box contentVisible{0, 100, 800, 600};
  const wlr_box outputBox{0, 0, 1920, 1080};
  constexpr int kBleed = 20;

  const wlr_box clip = umbriel::blurClipForOutput(nodeBox, contentVisible, outputBox, target, kBleed);

  CHECK_EQ(clip.x, kBleed);
  CHECK_EQ(clip.width, 800 - kBleed);
  // The other three edges are nowhere near the output bounds.
  CHECK_EQ(clip.y, 0);
  CHECK_EQ(clip.height, 600);
}

UMBRIEL_TEST(blurClipIsEmptyWhenTheViewIsOffTheOutput) {
  const wlr_box nodeBox{0, 0, 800, 600};
  const wlr_box target{3000, 100, 800, 600};
  const wlr_box contentVisible{3000, 100, 800, 600};
  const wlr_box outputBox{0, 0, 1920, 1080};

  const wlr_box clip = umbriel::blurClipForOutput(nodeBox, contentVisible, outputBox, target, 0);

  CHECK(clip.width <= 0 || clip.height <= 0);
}

UMBRIEL_TEST(blurClipIsEmptyWhenTheBleedSwallowsIt) {
  // A sliver narrower than twice the bleed has nothing safe left to sample.
  const wlr_box nodeBox{0, 0, 10, 600};
  const wlr_box target{0, 0, 10, 600};
  const wlr_box contentVisible{0, 0, 10, 600};
  const wlr_box outputBox{0, 0, 10, 600};

  const wlr_box clip = umbriel::blurClipForOutput(nodeBox, contentVisible, outputBox, target, 40);

  CHECK(clip.width <= 0 || clip.height <= 0);
}

UMBRIEL_TEST(blurClipTrimsToThePartOnThisOutput) {
  // Half the view hangs off the right edge of a 1920 output.
  const wlr_box nodeBox{0, 0, 800, 600};
  const wlr_box target{1520, 100, 800, 600};
  const wlr_box contentVisible{1520, 100, 400, 600};
  const wlr_box outputBox{0, 0, 1920, 1080};

  const wlr_box clip = umbriel::blurClipForOutput(nodeBox, contentVisible, outputBox, target, 0);

  CHECK_EQ(clip.x, 0);
  CHECK_EQ(clip.width, 400);
}

UMBRIEL_TEST(fullscreenCentersASmallerBuffer) {
  // A client that has not yet committed the fullscreen size sits centered in the
  // tile rather than pinned to a corner.
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, 1280), 320);
  CHECK_EQ(umbriel::fullscreenCenterOffset(1080, 800), 140);
}

UMBRIEL_TEST(fullscreenCropsAnOversizedBufferEqually) {
  // Negative offset: the buffer is wider than the output, so it loses the same
  // amount from each side instead of being scaled.
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, 2560), -320);
}

UMBRIEL_TEST(fullscreenOffsetIsZeroWithoutGeometry) {
  // A client with no committed geometry must not be shoved half an output over.
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, 0), 0);
  CHECK_EQ(umbriel::fullscreenCenterOffset(1920, -1), 0);
}

UMBRIEL_TEST(cropSelectsTheWholeBufferWhenNothingIsHidden) {
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  // Presented at the committed size, fully visible: the source is the whole buffer.
  const wlr_box content{0, 0, 1200, 900};
  const wlr_box clip{0, 0, 1200, 900};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.x), 0);
  CHECK_EQ(static_cast<int>(src.y), 0);
  CHECK_EQ(static_cast<int>(src.width), 1200);
  CHECK_EQ(static_cast<int>(src.height), 900);
}

UMBRIEL_TEST(cropScalesTheSourceByThePresentedShrink) {
  // Animating down to half size: each presented pixel covers two buffer pixels,
  // so a full-width visible box still selects the full-width source.
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 600, 450};
  const wlr_box clip{0, 0, 600, 450};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.width), 1200);
  CHECK_EQ(static_cast<int>(src.height), 900);
}

UMBRIEL_TEST(cropSelectsOnlyTheVisibleHalf) {
  // Presented at half size with only the left half on this output.
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 600, 450};
  const wlr_box clip{0, 0, 300, 450};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.x), 0);
  CHECK_EQ(static_cast<int>(src.width), 600);
}

UMBRIEL_TEST(cropHonoursTheSurfaceViewport) {
  // A client with a viewport: the buffer is twice the surface size, so every
  // surface coordinate maps to two buffer pixels on top of the presented scale.
  const wlr_fbox base{0, 0, 2400, 1800};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 1200, 900};
  const wlr_box clip{0, 0, 600, 900};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK_EQ(static_cast<int>(src.width), 1200);
  CHECK_EQ(static_cast<int>(src.height), 1800);
}

UMBRIEL_TEST(cropNeverReachesOutsideTheBuffer) {
  // A clip extending past the surface must clamp, not sample garbage.
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box content{0, 0, 1200, 900};
  const wlr_box clip{600, 0, 1200, 900};

  const wlr_fbox src = umbriel::croppedSourceBox(base, geometry, content, clip, 1200, 900);

  CHECK(src.x >= base.x);
  CHECK(src.x + src.width <= base.x + base.width);
  CHECK(src.y + src.height <= base.y + base.height);
}

UMBRIEL_TEST(cropIsEmptyForDegenerateInput) {
  const wlr_fbox base{0, 0, 1200, 900};
  const wlr_box geometry{0, 0, 1200, 900};
  const wlr_box clip{0, 0, 100, 100};

  // A zero-width presented box would divide by zero.
  CHECK(umbriel::croppedSourceBox(base, geometry, {0, 0, 0, 900}, clip, 1200, 900).width <= 0);
  // A surface that has committed nothing yet.
  CHECK(umbriel::croppedSourceBox(base, geometry, {0, 0, 600, 450}, clip, 0, 0).width <= 0);
  // A client with no geometry.
  CHECK(umbriel::croppedSourceBox(base, {0, 0, 0, 0}, {0, 0, 600, 450}, clip, 1200, 900).width <= 0);
}

int main() { return RUN_TESTS(); }
