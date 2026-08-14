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

Cards are clipped to the logical bounds of their output. If a workspace row
pushes a card beyond an output edge, the clipped edge loses its corner radius,
matching the treatment of a window clipped between outputs.

Each workspace has a rounded background behind its cards. The configured alpha
controls whether this is a light tint, a translucent panel, or an opaque fill.

## Dragging

A dragged card renders at half opacity. This multiplier combines with the
client's own surface alpha rather than replacing it, which keeps the insertion
preview visible through the card.

The scrolling layout previews insertion beside the actual column edges. The
dwindle layout previews the direction of the split before the card is dropped.

## Verification

The relevant checks are:

- [`tests/harness/checks/100_overview.sh`](../../tests/harness/checks/100_overview.sh)
  for overview interaction and workspace navigation.
- [`tests/harness/checks/107_drag_opacity.sh`](../../tests/harness/checks/107_drag_opacity.sh)
  for composed drag opacity.
- [`tests/output_clip.cpp`](../../tests/output_clip.cpp) for output clipping in
  logical coordinates.
