#pragma once

namespace umbriel {
  // A client's maximize request targets the edges when already edges-maximized, or when configured.
  [[nodiscard]] constexpr bool maximizeRequestTargetsEdges(bool edgesActive, bool configTargetsEdges) {
    return edgesActive || configTargetsEdges;
  }

} // namespace umbriel
