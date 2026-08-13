#pragma once

#include <cstdint>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {

  class Server;
  class View;
  class Workspace;

  struct DropColumnWidth {
    double fraction = 0.5;
    bool fullWidth = false;
  };

  // Where a dragged tile would land, plus the world-space hint rectangle.
  // Scrolling: row >= 0 inserts into column `column` at that row; row < 0 opens
  // a new column at gap index `column`. Dwindle: `view`/`edge` name a
  // directional split; when there is no splittable leaf they are null/0 and
  // `column` falls back to the append index. hintBox is clamped to the target
  // output's usable area; zero-sized means "draw nothing".
  struct DropTarget {
    Workspace* workspace = nullptr;
    int column = -1;
    int row = -1;
    View* view = nullptr;
    uint32_t edge = 0;
    wlr_box hintBox{};
  };

  // `scroll` is the horizontal offset the caller renders the workspace at.
  // `worldX`/`worldY` are layout coordinates in that same frame. Derives the
  // usable area from workspace.group()->output() internally.
  [[nodiscard]] DropTarget
  computeDropTarget(Workspace& workspace, double worldX, double worldY, const View* excludedView);

  // Inserts `view` at `drop` in `target` and focuses it. `columnWidth` restores
  // a detached scrolling column before the target is arranged.
  // The caller has already detached `view` from its source layout.
  void applyDrop(
      Server& server, View& view, Workspace& target, const DropTarget& drop, const DropColumnWidth* columnWidth,
      bool animate
  );

} // namespace umbriel
