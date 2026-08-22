#pragma once

#include <cstddef>
#include <optional>
#include <span>

namespace umbriel {

  struct OutputBox {
    int x;
    int y;
    int width;
    int height;
  };

  enum class OutputDirection {
    Left,
    Right,
    Up,
    Down,
  };

  // Select the nearest output in `direction` from `reference`. The point is in
  // layout coordinates and only affects which matching output is nearest.
  [[nodiscard]] std::optional<size_t> adjacentOutputIndex(
      std::span<const OutputBox> boxes, size_t reference, OutputDirection direction, double refX, double refY
  );

} // namespace umbriel
