# Xwayland input stability

Umbriel supports X11 clients through xwayland-satellite, which acts as both
the Wayland client owning every X11 surface and the window manager of its own
X server. Two satellite behaviors make compositor-side geometry churn
dangerous, and several Umbriel invariants exist purely to protect against
them. Breaking any of these reintroduces a bug class where an X11 game's
mouse input dies in part of the screen while everything looks correct.

## Satellite latches wl_surface.enter forever

Satellite positions each X11 window in its X coordinate space at the origin
of the output the surface last entered (`logical_position - global_min`), and
sets the surface's pointer scale from that output. `wl_surface.leave` never
reverts either one; only the next `enter` does. A single enter event for the
wrong output therefore permanently corrupts the window's X position and
pointer mapping until the surface happens to fully leave and re-enter its
real output.

wlroots emits enter/leave from scene-node visibility with a 10% overlap
threshold. Two situations cross it:

- Unanimated snap moves (`View::setPosition`) jump a node while its previous
  output clip still applies, exposing the overhang for one scene update
  (observed: a strip snap during a fullscreen fight re-homed a game to the
  neighboring output's origin).
- Interactive drags legitimately span monitors; every boundary graze latches
  the neighbor's origin and scale mid-drag. This is accepted as inherent to
  a drag (the drop's final enter re-homes correctly), see below.

Defense:

**Snap pre-clips** (`View::clipForSnapMove`): before an unanimated move, the
surface is clipped to the region visible on the home output at both
endpoints, so even the single scene update inside
`wlr_scene_node_set_position` cannot graze a neighbor.

A rejected approach, for whoever considers it next: pinning scene nodes to a
single output at the SceneFX level (filtering `active_outputs` in
`update_node_update_outputs`) does suppress the enter/leave churn, but it
divorces output membership from rendering. `wlr_scene_output_build_state`
asserts on buffers rendered on an output they are not members of (SIGABRT
when a pinned window is dragged fully onto another output), and anything
keyed on `primary_output` (drop attribution, frame scheduling, dmabuf
feedback) silently misbehaves. Membership must stay derived from real scene
geometry.

## X11 games never survive a windowed resize round trip

A fake-fullscreen game (borderless window at output size) that receives a
compositor-imposed windowed size, then returns to fullscreen, keeps a stale
mouse mapping: X geometry, stacking, focus, and event delivery all recover,
but hover and clicks die outside the transient size. This is upstream
Wine/satellite behavior, reproduced identically under niri; the boundary of
the dead zone always equals whatever transient size the compositor sent.

Umbriel therefore guarantees that compositor-driven fullscreen round trips
never emit a size change:

1. **Float toggle** (`View::setFloating(true)`) keeps the current size when
   floating a fullscreen window (no configure), and records
   `m_refullscreenOnTile`.
2. **Re-tile** (`View::setFloating(false)`) restores fullscreen before the
   layout attach when that flag is set, so arrange sizes the column to the
   full output the client already has. A client that chose windowed mode
   itself clears the flag (`setFullscreen(false)` from any other path) and
   re-tiles as a regular column.
3. **Unfullscreen grace** (`m_pendingUnfullscreenSize`, xwayland views only):
   a compositor-initiated unfullscreen goes out with size 0x0 (client picks
   its size, per xdg-shell), the view keeps its fullscreen layout slot and
   presentation (`View::layoutFullscreen`), and `Workspace::arrange`
   withholds the column size for `kUnfullscreenGraceMsec`. Acks prove
   nothing (satellite acks instantly), and the game observably reacts only
   when an actual resize pokes it, so the outcomes are:
   - the client re-requests fullscreen: the grace cancels in
     `setFullscreen(true)`, zero resizes;
   - the client commits a different geometry: it accepted windowed mode and
     tiles immediately;
   - the grace expires untouched: the client ignored the state change, and
     fullscreen is re-asserted rather than forcing a resize that would kill
     its input. This mirrors the observed niri behavior where such games
     effectively cannot be unfullscreened.
   Client-initiated unfullscreen requests skip the grace (the client wants
   windowed mode), and Wayland-native views keep immediate column sizing:
   they survive resizes, and many keep their size on 0x0, which would
   otherwise bounce them back to fullscreen.

Real migrations (dropping a window on a differently scaled output) still
resize by necessity and can still break fragile games; that is upstream
behavior, not something the compositor can mask.

## Verifying changes here

The headless harness cannot exercise satellite or multi-output X coordinate
spaces, so changes to these paths need a running session with an X11 game
(Steam plus any fake-fullscreen title reproduces within two toggles). The
signature to watch for on the game's X window during any toggle is a
ConfigureNotify pair through a non-fullscreen size; a passive
`StructureNotifyMask` monitor on `DISPLAY=:0` shows it directly.
