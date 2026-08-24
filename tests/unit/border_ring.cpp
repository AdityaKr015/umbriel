#include "view/border_ring.h"

#include "check.h"

using umbriel::expandedRadius;
using umbriel::makeBorderRing;

UMBRIEL_TEST(expandedRadiusKeepsSquareCornersSquare) {
  CHECK_EQ(expandedRadius(0, 4), 0);
  CHECK_EQ(expandedRadius(0, 0), 0);
  CHECK_EQ(expandedRadius(10, 4), 14);
}

UMBRIEL_TEST(ringUsesOneExpandedRectangle) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 120;
  constexpr int kThickness = 4;
  const auto ring = makeBorderRing(kWidth, kHeight, 10, kThickness);

  CHECK_EQ(ring.box.x, -kThickness);
  CHECK_EQ(ring.box.y, -kThickness);
  CHECK_EQ(ring.box.width, kWidth + 2 * kThickness);
  CHECK_EQ(ring.box.height, kHeight + 2 * kThickness);
}

UMBRIEL_TEST(ringHoleMatchesTheWindow) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 120;
  constexpr int kThickness = 4;
  const auto ring = makeBorderRing(kWidth, kHeight, 10, kThickness);

  CHECK_EQ(ring.hole.x, kThickness);
  CHECK_EQ(ring.hole.y, kThickness);
  CHECK_EQ(ring.hole.width, kWidth);
  CHECK_EQ(ring.hole.height, kHeight);
}

UMBRIEL_TEST(ringCurvesStayConcentric) {
  constexpr int kRadius = 10;
  constexpr int kThickness = 4;
  const auto ring = makeBorderRing(200, 120, kRadius, kThickness);

  CHECK_EQ(static_cast<int>(ring.outer.top_left), kRadius + kThickness);
  CHECK_EQ(static_cast<int>(ring.outer.top_right), kRadius + kThickness);
  CHECK_EQ(static_cast<int>(ring.outer.bottom_right), kRadius + kThickness);
  CHECK_EQ(static_cast<int>(ring.outer.bottom_left), kRadius + kThickness);
  CHECK_EQ(static_cast<int>(ring.inner.top_left), kRadius);
  CHECK_EQ(static_cast<int>(ring.inner.top_right), kRadius);
  CHECK_EQ(static_cast<int>(ring.inner.bottom_right), kRadius);
  CHECK_EQ(static_cast<int>(ring.inner.bottom_left), kRadius);
}

UMBRIEL_TEST(squareRingKeepsBothCurvesSquare) {
  const auto ring = makeBorderRing(200, 120, 0, 4);

  CHECK_EQ(static_cast<int>(ring.outer.top_left), 0);
  CHECK_EQ(static_cast<int>(ring.outer.bottom_right), 0);
  CHECK_EQ(static_cast<int>(ring.inner.top_left), 0);
  CHECK_EQ(static_cast<int>(ring.inner.bottom_right), 0);
}

int main() { return RUN_TESTS(); }
