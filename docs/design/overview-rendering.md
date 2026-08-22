# Overview rendering

This note records rendering and interaction details that are too specific for
the main configuration guide but remain part of Umbriel's observable behavior.

## Live content

Overview cards display live window content. The real workspace windows are
hidden while the overview is open, so wheel steps, arrow keys, and 3-finger
swipes move one workspace at a time instead of sliding the live workspace.

Transparent windows keep their window-rule blur throughout the zoom
transition.

## Animation ownership

Cards use separate scene buffers because the overview scales and clips each
window into a workspace row. They do not own a second window animation state.
Every `View` remains the authority for its currently presented position, size,
and opacity, including for a hidden workspace while the overview is open. The
overview projects that presented box through its row and zoom transform.

Only overview-specific motion lives in `Overview`: opening and closing zoom,
filmstrip scrolling, card dragging, and drop hints. Layout movement, resize,
and fade transitions continue to advance in `View`, so the overview and the
normal workspace cannot settle through different paths.

## Decoration and clipping

Cards carry the same inner border, outer border, and corner radius as their
windows. These values scale with the card.

Each output's overview tree carries a `wlr_scene_tree_set_clip` of that
output's logical bounds, the same primitive windows use. A workspace row that
pushes a card past an output edge is scissored there: cards, border rings,
workspace backgrounds and blur are all contained by that one clip, and none of
them trims its own geometry. The dragged card is reparented out to the
unclipped overview root so it can span outputs, exactly as a dragged window
does.

Each workspace has a rounded background behind its cards. The configured alpha
controls whether this is a light tint, a translucent panel, or an opaque fill.

The focused border tracks the workspace's focused view, so each row shows where
it will land when zoomed into. Closing the focused window reassigns focus to a
remaining window on that workspace while the overview stays open; the border
moves with it.

## Dragging

A dragged card renders at 0.75 opacity (`View::kDragOpacity`). This multiplier
combines with the client's own surface alpha rather than replacing it, which
keeps the insertion preview visible through the card.

The scrolling layout previews insertion beside the actual column edges. For an
overflowing strip, prepend and append previews remain visible at the output
edges. The dwindle layout previews the direction of the split before the card
is dropped.

## Verification

The relevant checks are:

- [`tests/harness/checks/100_overview.sh`](../../tests/harness/checks/100_overview.sh)
  for overview interaction and workspace navigation.
- [`tests/harness/checks/101_external_drag.sh`](../../tests/harness/checks/101_external_drag.sh)
  for client drag ownership during overview activation.
- [`tests/harness/checks/107_drag_opacity.sh`](../../tests/harness/checks/107_drag_opacity.sh)
  for composed drag opacity.
- [`tests/harness/checks/111_drag_left_hint.sh`](../../tests/harness/checks/111_drag_left_hint.sh)
  for the visible prepend target on an overflowing scrolling strip.
- [`tests/harness/checks/112_overview_refocus.sh`](../../tests/harness/checks/112_overview_refocus.sh)
  for focus reassignment when the focused window closes in the overview.
- [`tests/harness/two-output-containment.sh`](../../tests/harness/two-output-containment.sh)
  for cards staying off a neighbouring output, overview included.
- [`tests/presented_crop.cpp`](../../tests/presented_crop.cpp) for the
  presented-crop math shared with window presentation.
