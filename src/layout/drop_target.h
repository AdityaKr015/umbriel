#pragma once

struct wlr_box;

namespace umbriel {

  class Workspace;

  // Where a dropped window lands in a scrolling workspace.
  // `row >= 0` inserts into the existing column `column` at that row;
  // `row < 0` opens a new column at gap index `column`.
  struct ScrollingDropTarget {
    int column = 0;
    int row = -1;
  };

  // `usable` is the target output's usable area and `scroll` the horizontal
  // offset the caller renders the workspace at (visual scroll for a live drag,
  // the layout's authoritative scroll for overview cards). `worldX`/`worldY`
  // are layout coordinates already mapped into that same frame.
  [[nodiscard]] ScrollingDropTarget computeScrollingDropTarget(
      const Workspace& workspace, const wlr_box& usable, double scroll, double worldX, double worldY
  );

} // namespace umbriel
