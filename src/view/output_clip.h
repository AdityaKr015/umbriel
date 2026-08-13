#pragma once

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  [[nodiscard]] constexpr wlr_box surfaceClipForOutput(
      const wlr_box& geometry, const wlr_box& content, const wlr_box& visible, int fullscreenOffsetX,
      int fullscreenOffsetY
  ) {
    return {
        .x = geometry.x + visible.x - content.x - fullscreenOffsetX,
        .y = geometry.y + visible.y - content.y - fullscreenOffsetY,
        .width = visible.width,
        .height = visible.height,
    };
  }
} // namespace umbriel
