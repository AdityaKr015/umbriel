#include "check.h"
#include "config/config.h"
#include "layout/dwindle.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath>
extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}
// clang-format on

using umbriel::DwindleLayout;
using umbriel::LayoutConstraints;
using umbriel::ResolvedLayoutConfig;
using umbriel::View;

namespace {

  // The layout treats View as an opaque identity; these are addresses, never
  // dereferenced.
  View* stub(int id) { return reinterpret_cast<View*>(static_cast<uintptr_t>(0x2000 + (id * 0x10))); }

  ResolvedLayoutConfig dwindleConfig() {
    ResolvedLayoutConfig config;
    config.mode = umbriel::LayoutMode::Dwindle;
    config.gap = 8;
    config.totalGap = 12;
    config.edgePad = 10;
    return config;
  }

  constexpr wlr_box kUsable{0, 0, 1280, 720};

  struct Fixture {
    ResolvedLayoutConfig config = dwindleConfig();
    DwindleLayout layout;

    Fixture() { layout.setConfig(&config); }

    // Deliberately no arrange() between inserts: Workspace::applyConfig
    // batch-inserts on a fresh layout when the layout mode changes.
    void addLeaves(int count) {
      for (int i = 0; i < count; ++i) {
        layout.insertView(stub(i), i);
      }
    }
  };

  bool overlaps(const wlr_box& a, const wlr_box& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
  }

} // namespace

// ---- tree bookkeeping ----

UMBRIEL_TEST(firstLeafFillsTheUsableArea) {
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.arrange(kUsable);

  const wlr_box box = fixture.layout.targetBox(stub(0));
  CHECK_EQ(box.x, kUsable.x + fixture.config.edgePad);
  CHECK_EQ(box.y, kUsable.y + fixture.config.edgePad);
  CHECK_EQ(box.width, kUsable.width - 2 * fixture.config.edgePad);
  CHECK_EQ(box.height, kUsable.height - 2 * fixture.config.edgePad);
}

UMBRIEL_TEST(insertBuildsOneLeafPerView) {
  Fixture fixture;
  fixture.addLeaves(4);
  CHECK_EQ(fixture.layout.columns().size(), size_t{4});
  for (int i = 0; i < 4; ++i) {
    CHECK(fixture.layout.columnOf(stub(i)) >= 0);
  }
}

UMBRIEL_TEST(insertIsIdempotentPerView) {
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.insertView(stub(0), 0);
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});
}

UMBRIEL_TEST(batchInsertWithoutArrangeKeepsEveryView) {
  // Workspace::applyConfig creates a fresh layout and re-inserts every tiled
  // view in a loop, arranging only afterwards. insertView locates its target
  // leaf through the flat-column cache, so if a structural edit leaves that
  // cache stale, every view after the first is silently dropped.
  Fixture fixture;
  for (int i = 0; i < 5; ++i) {
    fixture.layout.insertView(stub(i), static_cast<int>(fixture.layout.columns().size()));
  }
  CHECK_EQ(fixture.layout.columns().size(), size_t{5});

  fixture.layout.arrange(kUsable);
  for (int i = 0; i < 5; ++i) {
    CHECK(fixture.layout.columnOf(stub(i)) >= 0);
    CHECK(fixture.layout.targetBox(stub(i)).width > 0);
  }
}

UMBRIEL_TEST(swapOperationsRefreshTheColumnMapping) {
  // consumeLeft, expelRight, moveViewVertical, and moveColumn reassign leaf
  // views. The column mapping must be current straight afterwards, without
  // waiting for the next arrange().
  Fixture fixture;
  fixture.addLeaves(2);
  const int first = fixture.layout.columnOf(stub(0));
  const int second = fixture.layout.columnOf(stub(1));
  CHECK(first >= 0);
  CHECK(second >= 0);

  fixture.layout.moveColumn(first, second);
  CHECK_EQ(fixture.layout.columnOf(stub(0)), second);
  CHECK_EQ(fixture.layout.columnOf(stub(1)), first);
}

UMBRIEL_TEST(unknownViewHasNoColumn) {
  Fixture fixture;
  fixture.addLeaves(2);
  CHECK_EQ(fixture.layout.columnOf(stub(99)), -1);
  CHECK_EQ(fixture.layout.columnOf(nullptr), -1);
}

UMBRIEL_TEST(everyLeafIsOnRowZero) {
  // Dwindle has no row concept; the whole tree is flattened to columns.
  Fixture fixture;
  fixture.addLeaves(3);
  for (int i = 0; i < 3; ++i) {
    CHECK_EQ(fixture.layout.rowOf(stub(i)), 0);
  }
}

UMBRIEL_TEST(removingCollapsesTheSiblingIntoTheParent) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.removeView(stub(1));
  CHECK_EQ(fixture.layout.columns().size(), size_t{1});

  fixture.layout.arrange(kUsable);
  // The survivor reclaims the whole area rather than keeping half of a split.
  const wlr_box box = fixture.layout.targetBox(stub(0));
  CHECK_EQ(box.width, kUsable.width - 2 * fixture.config.edgePad);
  CHECK_EQ(box.height, kUsable.height - 2 * fixture.config.edgePad);
}

UMBRIEL_TEST(removingEveryViewEmptiesTheTree) {
  Fixture fixture;
  fixture.addLeaves(3);
  for (int i = 0; i < 3; ++i) {
    fixture.layout.removeView(stub(i));
  }
  CHECK_EQ(fixture.layout.columns().size(), size_t{0});
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.targetBox(stub(0)).width, 0);
}

UMBRIEL_TEST(removingAnUnknownViewIsHarmless) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.removeView(stub(99));
  fixture.layout.removeView(nullptr);
  CHECK_EQ(fixture.layout.columns().size(), size_t{2});
}

// ---- tiling geometry ----

UMBRIEL_TEST(twoLeavesSplitTheAreaWithoutOverlapping) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));

  CHECK(!overlaps(first, second));
  CHECK(first.width > 0);
  CHECK(second.width > 0);
  // A 1280x720 area splits horizontally first, so the two share a top edge.
  CHECK_EQ(first.y, second.y);
  CHECK_EQ(first.height, second.height);
}

UMBRIEL_TEST(leavesNeverOverlapOrLeaveTheUsableArea) {
  Fixture fixture;
  fixture.addLeaves(6);
  fixture.layout.arrange(kUsable);

  const wlr_box bounds{
      kUsable.x + fixture.config.edgePad,
      kUsable.y + fixture.config.edgePad,
      kUsable.width - 2 * fixture.config.edgePad,
      kUsable.height - 2 * fixture.config.edgePad,
  };

  for (int i = 0; i < 6; ++i) {
    const wlr_box box = fixture.layout.targetBox(stub(i));
    CHECK(box.width > 0);
    CHECK(box.height > 0);
    CHECK(box.x >= bounds.x);
    CHECK(box.y >= bounds.y);
    CHECK(box.x + box.width <= bounds.x + bounds.width);
    CHECK(box.y + box.height <= bounds.y + bounds.height);

    for (int j = i + 1; j < 6; ++j) {
      CHECK(!overlaps(box, fixture.layout.targetBox(stub(j))));
    }
  }
}

UMBRIEL_TEST(splitsAlternateOrientation) {
  Fixture fixture;
  fixture.addLeaves(3);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));
  const wlr_box third = fixture.layout.targetBox(stub(2));

  // First split is horizontal (side by side), the second splits that half
  // vertically (stacked), so leaves 1 and 2 share a column.
  CHECK(second.x == third.x);
  CHECK(second.y != third.y);
  CHECK(first.x != second.x);
}

UMBRIEL_TEST(gapsSeparateSiblings) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const wlr_box second = fixture.layout.targetBox(stub(1));
  CHECK_EQ(second.x - (first.x + first.width), fixture.config.totalGap);
}

UMBRIEL_TEST(targetBoxOfAnUnknownViewIsEmpty) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  const wlr_box box = fixture.layout.targetBox(stub(99));
  CHECK_EQ(box.width, 0);
  CHECK_EQ(box.height, 0);
}

UMBRIEL_TEST(arrangeOnAnEmptyTreeIsHarmless) {
  Fixture fixture;
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.columns().size(), size_t{0});
}

// ---- hit testing ----

UMBRIEL_TEST(leafIndexAtFindsTheLeafUnderAPoint) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  const wlr_box first = fixture.layout.targetBox(stub(0));
  const int index = fixture.layout.leafIndexAt(first.x + first.width / 2.0, first.y + first.height / 2.0);
  CHECK(index >= 0);
  CHECK_EQ(fixture.layout.targetBoxByIndex(index).x, first.x);
}

UMBRIEL_TEST(leafIndexAtMissesOutsideTheArea) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.leafIndexAt(-500, -500), -1);
  CHECK_EQ(fixture.layout.leafIndexAt(99999, 99999), -1);
}

// ---- resize boundaries ----

UMBRIEL_TEST(onlyInternalEdgesAreResizable) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);

  // Screen-facing edges are not resizable; the shared boundary is.
  const uint32_t firstEdges = fixture.layout.resizableEdges(stub(0));
  CHECK_EQ(firstEdges & WLR_EDGE_LEFT, uint32_t{0});
  CHECK(firstEdges & WLR_EDGE_RIGHT);

  const uint32_t secondEdges = fixture.layout.resizableEdges(stub(1));
  CHECK(secondEdges & WLR_EDGE_LEFT);
  CHECK_EQ(secondEdges & WLR_EDGE_RIGHT, uint32_t{0});
}

UMBRIEL_TEST(aLoneLeafHasNoResizableEdges) {
  Fixture fixture;
  fixture.addLeaves(1);
  fixture.layout.arrange(kUsable);
  CHECK_EQ(fixture.layout.resizableEdges(stub(0)), uint32_t{0});
}

UMBRIEL_TEST(setResizeBoundaryMovesTheSharedEdge) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  const int before = fixture.layout.targetBox(stub(0)).width;

  double ratio = 0.0;
  double span = 0.0;
  CHECK(fixture.layout.resizeBoundary(stub(0), WLR_EDGE_RIGHT, &ratio, &span));
  CHECK(std::fabs(ratio - 0.5) < 1e-6);
  CHECK(span > 0.0);

  CHECK(fixture.layout.setResizeBoundary(stub(0), WLR_EDGE_RIGHT, 0.75));
  fixture.layout.arrange(kUsable);
  CHECK(fixture.layout.targetBox(stub(0)).width > before);

  // The sibling gives up exactly what the first leaf gained.
  CHECK(!overlaps(fixture.layout.targetBox(stub(0)), fixture.layout.targetBox(stub(1))));
}

UMBRIEL_TEST(resizeBoundaryRejectsAScreenFacingEdge) {
  Fixture fixture;
  fixture.addLeaves(2);
  fixture.layout.arrange(kUsable);
  double ratio = 0.0;
  double span = 0.0;
  CHECK(!fixture.layout.resizeBoundary(stub(0), WLR_EDGE_LEFT, &ratio, &span));
  CHECK(!fixture.layout.setResizeBoundary(stub(0), WLR_EDGE_LEFT, 0.75));
}

// ---- scrolling-only API stays inert ----

UMBRIEL_TEST(scrollingOnlyOperationsAreInert) {
  // Dwindle inherits the scrolling-specific parts of the Layout interface and
  // answers them with defaults. Pinning that here so the behavior is visible:
  // the interface is wider than any single layout implements.
  Fixture fixture;
  fixture.addLeaves(2);

  CHECK_EQ(fixture.layout.scroll(), 0.0);
  fixture.layout.setScroll(500);
  CHECK_EQ(fixture.layout.scroll(), 0.0);
  CHECK_EQ(fixture.layout.maxScroll(1260), 0);
  CHECK_EQ(fixture.layout.columnX(0, 1260), 0);
  CHECK_EQ(fixture.layout.columnWidth(0, 1260), 0);
  CHECK(!fixture.layout.isFullWidth(0));
  CHECK(!fixture.layout.setRowBoundary(0, 0, 0.5, 0.5));
  CHECK(!fixture.layout.setHeightWeight(0, 0, 2.0));
}

int main() { return RUN_TESTS(); }
