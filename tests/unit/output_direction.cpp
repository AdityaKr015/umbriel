#include "check.h"
#include "output/direction.h"

#include <array>

using umbriel::adjacentOutputIndex;
using umbriel::OutputBox;
using umbriel::OutputDirection;

UMBRIEL_TEST(scaledOutputsMayOverlapAtTheirSharedEdge) {
  // Reported layout: a 3840x2160 output at scale 1.6, with the scaled laptop
  // panel centered below it. Rounding leaves a two-logical-pixel overlap.
  constexpr std::array boxes{
      OutputBox{0, 0, 2400, 1350},
      OutputBox{416, 1348, 1646, 1066},
  };

  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Down, 1200, 675), std::optional<size_t>{1});
  CHECK_EQ(adjacentOutputIndex(boxes, 1, OutputDirection::Up, 1239, 1881), std::optional<size_t>{0});
  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Left, 1200, 675), std::optional<size_t>{});
  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Right, 1200, 675), std::optional<size_t>{});
}

UMBRIEL_TEST(horizontalNeighborsMayOverlapAfterRounding) {
  constexpr std::array boxes{
      OutputBox{0, 0, 1000, 800},
      OutputBox{998, 50, 800, 700},
  };

  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Right, 500, 400), std::optional<size_t>{1});
  CHECK_EQ(adjacentOutputIndex(boxes, 1, OutputDirection::Left, 1398, 400), std::optional<size_t>{0});
  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Down, 500, 400), std::optional<size_t>{});
}

UMBRIEL_TEST(nearestDirectionalOutputWins) {
  constexpr std::array boxes{
      OutputBox{0, 0, 1000, 800},
      OutputBox{1000, 0, 800, 800},
      OutputBox{1800, 0, 800, 800},
  };

  CHECK_EQ(adjacentOutputIndex(boxes, 0, OutputDirection::Right, 500, 400), std::optional<size_t>{1});
}

int main() { return RUN_TESTS(); }
