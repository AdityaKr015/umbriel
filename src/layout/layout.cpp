#include "layout/layout.h"

#include "config/config.h"
#include "layout/dwindle.h"
#include "layout/scrolling.h"

#include <algorithm>
#include <cmath>
#include <memory>

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

namespace umbriel {

  wlr_box Layout::contentArea(const wlr_box& usable) const {
    const int edgePad = m_config != nullptr ? m_config->edgePad : 0;
    return {
        .x = usable.x + edgePad,
        .y = usable.y + edgePad,
        .width = std::max(1, usable.width - 2 * edgePad),
        .height = std::max(1, usable.height - 2 * edgePad),
    };
  }

  int Layout::fractionalWidth(int viewportPrimary, double fraction) const {
    const int gap = m_config != nullptr ? m_config->totalGap : 0;
    return std::max(1, static_cast<int>(std::lround(fraction * (viewportPrimary + gap) - gap)));
  }

  std::unique_ptr<Layout> createLayout(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Dwindle:
      return std::make_unique<DwindleLayout>();
    case LayoutMode::Scrolling:
    default:
      return std::make_unique<ScrollingLayout>();
    }
  }

  uint32_t resizeEdgesForPoint(const wlr_box& box, double cx, double cy) {
    const double px = cx - box.x;
    const double py = cy - box.y;
    uint32_t edges = 0;
    if (px < box.width / 3.0) {
      edges |= WLR_EDGE_LEFT;
    } else if (px > 2.0 * box.width / 3.0) {
      edges |= WLR_EDGE_RIGHT;
    }
    if (py < box.height / 3.0) {
      edges |= WLR_EDGE_TOP;
    } else if (py > 2.0 * box.height / 3.0) {
      edges |= WLR_EDGE_BOTTOM;
    }
    return edges;
  }

} // namespace umbriel
