#include "view/border_ring.h"

#include "check.h"

using umbriel::BorderEdge;
using umbriel::expandedRadius;
using umbriel::makeBorderRing;

namespace {

  // Index into the ring returned by makeBorderRing.
  enum Edge { Top = 0, Bottom = 1, Left = 2, Right = 3 };

  bool covers(const wlr_box& box, int x, int y) {
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
  }

  // A point is painted if some edge covers it and that edge's hole does not.
  bool painted(const std::array<BorderEdge, 4>& ring, int x, int y) {
    for (const auto& edge : ring) {
      if (!covers(edge.box, x, y)) {
        continue;
      }
      if (edge.hasHole && covers(edge.hole, x - edge.box.x, y - edge.box.y)) {
        continue;
      }
      return true;
    }
    return false;
  }

} // namespace

UMBRIEL_TEST(expandedRadiusKeepsSquareCornersSquare) {
  CHECK_EQ(expandedRadius(0, 4), 0);
  CHECK_EQ(expandedRadius(0, 0), 0);
  // A ring outside a rounded rect must grow to stay concentric.
  CHECK_EQ(expandedRadius(10, 4), 14);
}

UMBRIEL_TEST(ringNeverHasNegativeExtents) {
  // Content smaller than the corners it is asked to round is the case that
  // produced negative side heights before the clamp.
  for (int w : {0, 1, 5, 40, 800}) {
    for (int h : {0, 1, 5, 40, 800}) {
      for (int radius : {0, 3, 12}) {
        for (int thickness : {0, 1, 6}) {
          const auto ring = makeBorderRing(w, h, radius, thickness);
          for (const auto& edge : ring) {
            CHECK(edge.box.width >= 0);
            CHECK(edge.box.height >= 0);
          }
        }
      }
    }
  }
}

UMBRIEL_TEST(ringEnclosesTheContentOnEveryside) {
  constexpr int kW = 200;
  constexpr int kH = 120;
  constexpr int kRadius = 10;
  constexpr int kThickness = 4;
  const auto ring = makeBorderRing(kW, kH, kRadius, kThickness);

  // Walk the band just outside the surface. Every point of it must be painted,
  // or the window would show a gap in its border.
  for (int x = 0; x < kW; ++x) {
    CHECK(painted(ring, x, -1)); // directly above
    CHECK(painted(ring, x, kH)); // directly below
  }
  for (int y = 0; y < kH; ++y) {
    CHECK(painted(ring, -1, y)); // directly left
    CHECK(painted(ring, kW, y)); // directly right
  }
}

UMBRIEL_TEST(ringLeavesTheSurfaceItselfUnpainted) {
  constexpr int kW = 200;
  constexpr int kH = 120;
  const auto ring = makeBorderRing(kW, kH, 10, 4);

  // Away from the rounded corners the surface must be fully exposed; painting
  // there would cover the window's own content.
  for (int x = 12; x < kW - 12; ++x) {
    CHECK(!painted(ring, x, 0));
    CHECK(!painted(ring, x, kH - 1));
  }
  for (int y = 12; y < kH - 12; ++y) {
    CHECK(!painted(ring, 0, y));
    CHECK(!painted(ring, kW - 1, y));
  }
}

UMBRIEL_TEST(sidesSpanExactlyTheStraightRun) {
  const auto ring = makeBorderRing(200, 120, 10, 4);
  // Sides start where the top slab's rounded part ends and stop where the
  // bottom slab's begins, so the two must meet without overlapping.
  CHECK_EQ(ring[Left].box.y, 10);
  CHECK_EQ(ring[Left].box.height, 120 - 2 * 10);
  CHECK_EQ(ring[Left].box.y + ring[Left].box.height, ring[Bottom].box.y);
  CHECK_EQ(ring[Right].box.y, ring[Left].box.y);
  CHECK_EQ(ring[Right].box.height, ring[Left].box.height);
  // Sides are straight: no rounding, no hole.
  CHECK(!ring[Left].hasHole);
  CHECK(!ring[Right].hasHole);
}

UMBRIEL_TEST(sidesSitFlushAgainstTheSurface) {
  constexpr int kW = 200;
  constexpr int kThickness = 4;
  const auto ring = makeBorderRing(kW, 120, 10, kThickness);
  CHECK_EQ(ring[Left].box.x + ring[Left].box.width, 0);
  CHECK_EQ(ring[Right].box.x, kW);
  CHECK_EQ(ring[Left].box.width, kThickness);
  CHECK_EQ(ring[Right].box.width, kThickness);
}

UMBRIEL_TEST(slabsSpanTheFullDecoratedWidth) {
  constexpr int kW = 200;
  constexpr int kThickness = 4;
  const auto ring = makeBorderRing(kW, 120, 10, kThickness);
  for (Edge edge : {Top, Bottom}) {
    CHECK_EQ(ring[edge].box.x, -kThickness);
    CHECK_EQ(ring[edge].box.width, kW + 2 * kThickness);
    CHECK(ring[edge].hasHole);
    // The hole exposes the surface's full width, nothing more.
    CHECK_EQ(ring[edge].hole.width, kW);
  }
  CHECK_EQ(ring[Top].box.y, -kThickness);
}

UMBRIEL_TEST(zeroRadiusGivesSidesTheFullHeight) {
  constexpr int kH = 120;
  const auto ring = makeBorderRing(200, kH, 0, 4);
  CHECK_EQ(ring[Left].box.y, 0);
  CHECK_EQ(ring[Left].box.height, kH);
  // With square corners the outer radius stays zero too.
  CHECK_EQ(ring[Top].outer.top_left, 0);
  CHECK_EQ(ring[Top].outer.top_right, 0);
}

UMBRIEL_TEST(cornersGrowWithThickness) {
  constexpr int kRadius = 10;
  constexpr int kThickness = 4;
  const auto ring = makeBorderRing(200, 120, kRadius, kThickness);
  // Outer edge of the ring is concentric with the surface's rounding.
  CHECK_EQ(static_cast<int>(ring[Top].outer.top_left), kRadius + kThickness);
  CHECK_EQ(static_cast<int>(ring[Bottom].outer.bottom_right), kRadius + kThickness);
  // The hole tracks the surface's own radius, not the expanded one.
  CHECK_EQ(static_cast<int>(ring[Top].holeCorners.top_left), kRadius);
  CHECK_EQ(static_cast<int>(ring[Bottom].holeCorners.bottom_left), kRadius);
}

int main() { return RUN_TESTS(); }
