# Border rendering

Umbriel renders each decorated window with one SceneFX `wlr_scene_border` node.
The node owns both color bands and submits one draw through the dedicated
`border.frag` shader. The regular rounded-rectangle shader is not part of the
window border path.

## Geometry contract

The content box is authoritative. A border node receives:

- the content box and corner radius;
- the inner and outer logical widths;
- the inner and outer premultiplied colors;
- a raster box large enough for the complete stroke and antialias coverage.

`makeBorderRing` adds one transparent logical pixel outside the configured total
width. This margin does not affect layout geometry or the visible border width.
It only ensures that a fractional outer sample is not clipped by the scene-node
quad before the fragment shader can evaluate it.

Scene rendering scales both widths as floating-point values. The shader therefore
receives the requested physical thickness, such as 1.25 pixels, rather than an
independently rounded top, bottom, left, or right width.

## Fragment contract

`border.frag` evaluates one signed distance from the content box. Rounded
corners use a Euclidean rounded-rectangle distance. A zero radius uses an
$L_\infty$ rectangle distance so outward offset contours remain square instead
of acquiring an implicit radius from Euclidean corner distance. The same
distance drives:

1. inner-edge coverage;
2. the transition between inner and outer colors;
3. outer-edge coverage.

The distance function and antialias width are continuous across the entire
perimeter. Straight edges and rounded corners do not switch coordinate systems
or feather widths at their tangency. The two colors mix at their shared boundary
inside one fragment; they are never overlapping transparent scene nodes.

Both edge coverages are symmetric around their geometric boundaries. The outer
coverage uses the sum of the logical widths, while the color transition uses the
inner width. A zero outer width selects the inner color for the complete stroke.

## CPU clipping

SceneFX limits fragment work by subtracting areas that are certainly inside the
transparent content hole. Rounded corners must remain shader-owned.
`apply_clip_region` therefore subtracts a central horizontal and vertical cross,
not a diagonal approximation of the rounded hole. Integer truncation of a corner
approximation can otherwise remove an isolated fragment before the shader runs.

## Scene lifecycle

View decorations, overview cards, and their close-animation snapshots all copy
or animate one `wlr_scene_border`. Focus animation updates the inner color;
opacity animation updates both premultiplied colors. The outer color never needs
a second scene node or draw order.

## Regression coverage

- `365_fractional_border_coverage.sh` checks a one-logical-pixel border at scale
  1.25. The top and side must have equal opaque and fractional coverage.
- `722_subsurface_border_corner.sh` checks the straight-to-curve tangency, inner
  corner coverage, the two-color seam, and opaque outer-corner coverage against
  a full-window subsurface.
- `723_square_border_corner.sh` verifies that radius zero preserves the extreme
  outer-corner pixel instead of rounding it away.
- `border-ring` unit tests protect the transparent raster margin and content-hole
  geometry.

When changing border geometry or antialiasing, temporarily break the relevant
invariant and confirm its regression check fails at the intended sample.
