# Overview rendering

This note records rendering and interaction details that are too specific for
the main configuration guide but remain part of Umbriel's observable behavior.

## Live content

Overview cards display live window content. The real workspace windows are
hidden while the overview is open, so wheel steps, arrow keys, and 3-finger
swipes move one workspace at a time instead of sliding the live workspace.

Transparent windows keep their window-rule blur throughout the zoom
transition.

## Decoration and clipping

Cards carry the same inner border, outer border, and corner radius as their
windows. These values scale with the card.

Cards are clipped to the logical bounds of their output by their own manual
card clipping, not by the per-output scene roots windows live under. If a
workspace row pushes a card beyond an output edge, the clipped edge loses its
corner radius so the cut reads as a clip and not as a smaller rounded card.

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
- [`tests/output_clip.cpp`](../../tests/output_clip.cpp) for the card corner
  squaring and presented-crop math in logical coordinates.
