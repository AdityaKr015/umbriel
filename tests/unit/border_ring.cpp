#include "view/border_ring.h"

#include "check.h"

using umbriel::expandedRadius;
using umbriel::makeBorderRing;

UMBRIEL_TEST(expandedRadiusKeepsSquareCornersSquare) {
  CHECK_EQ(expandedRadius(0, 4), 0);
  CHECK_EQ(expandedRadius(0, 0), 0);
  CHECK_EQ(expandedRadius(10, 4), 14);
}

UMBRIEL_TEST(roundedRingIncludesRasterMargin) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 120;
  constexpr int kThickness = 4;
  constexpr int kExtent = kThickness + 1;
  const auto ring = makeBorderRing(kWidth, kHeight, 10, kThickness);

  CHECK_EQ(ring.box.x, -kExtent);
  CHECK_EQ(ring.box.y, -kExtent);
  CHECK_EQ(ring.box.width, kWidth + 2 * kExtent);
  CHECK_EQ(ring.box.height, kHeight + 2 * kExtent);
}

UMBRIEL_TEST(ringHoleMatchesTheWindow) {
  constexpr int kWidth = 200;
  constexpr int kHeight = 120;
  constexpr int kThickness = 4;
  constexpr int kExtent = kThickness + 1;
  const auto ring = makeBorderRing(kWidth, kHeight, 10, kThickness);

  CHECK_EQ(ring.hole.x, kExtent);
  CHECK_EQ(ring.hole.y, kExtent);
  CHECK_EQ(ring.hole.width, kWidth);
  CHECK_EQ(ring.hole.height, kHeight);
}

UMBRIEL_TEST(ringHoleUsesContentRadius) {
  constexpr int kRadius = 10;
  const auto ring = makeBorderRing(200, 120, kRadius, 4);

  CHECK_EQ(static_cast<int>(ring.inner.top_left), kRadius);
  CHECK_EQ(static_cast<int>(ring.inner.top_right), kRadius);
  CHECK_EQ(static_cast<int>(ring.inner.bottom_right), kRadius);
  CHECK_EQ(static_cast<int>(ring.inner.bottom_left), kRadius);
}

UMBRIEL_TEST(squareRingKeepsSquareContentHole) {
  const auto ring = makeBorderRing(200, 120, 0, 4);

  CHECK_EQ(static_cast<int>(ring.inner.top_left), 0);
  CHECK_EQ(static_cast<int>(ring.inner.bottom_right), 0);
  CHECK_EQ(ring.box.x, -5);
  CHECK_EQ(ring.hole.x, 5);
}

int main() { return RUN_TESTS(); }
