#include "check.h"
#include "config/config.h"

// Links the compositor library rather than the pure one, which is the point:
// resolveGlobalLayout lives in config.cpp alongside the config singleton, so
// this is also the proof that a test can reach compositor code without listing
// or recompiling any source.

UMBRIEL_TEST(defaultLayoutMetricsAreDerivedFromGapAndBorder) {
  // Everything that positions a window works in these two derived numbers, and
  // the layout tests and the harness both hardcode 12 and 10. This is where
  // that arithmetic actually comes from:
  //   totalGap = gap + 2 * totalBorderWidth   (a gap plus a border on each side)
  //   edgePad  = gap + totalBorderWidth       (one edge of the screen)
  const umbriel::ResolvedLayoutConfig layout = umbriel::resolveGlobalLayout();
  const umbriel::Config& config = umbriel::config();

  const int border = config.appearance.totalBorderWidth();
  CHECK_EQ(layout.gap, config.layout.gap);
  CHECK_EQ(layout.totalGap, layout.gap + 2 * border);
  CHECK_EQ(layout.edgePad, layout.gap + border);

  // The shipped defaults, which the other tests assume.
  CHECK_EQ(layout.gap, 8);
  CHECK_EQ(border, 2);
  CHECK_EQ(layout.totalGap, 12);
  CHECK_EQ(layout.edgePad, 10);
}

UMBRIEL_TEST(configHelpersAgreeWithTheResolvedLayout) {
  // Config exposes the same two numbers for callers that have no workspace yet.
  const umbriel::Config& config = umbriel::config();
  const umbriel::ResolvedLayoutConfig layout = umbriel::resolveGlobalLayout();
  CHECK_EQ(config.layoutGap(), layout.totalGap);
  CHECK_EQ(config.layoutEdgePad(), layout.edgePad);
}

int main() { return RUN_TESTS(); }
