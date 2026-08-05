#include "layout/layout.h"

#include "layout/dwindle.h"
#include "layout/scrolling.h"

#include <memory>

namespace umbriel {

  std::unique_ptr<Layout> createLayout(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Dwindle:
      return std::make_unique<DwindleLayout>();
    case LayoutMode::Scrolling:
    default:
      return std::make_unique<ScrollingLayout>();
    }
  }

} // namespace umbriel
