# Border rendering

Umbriel renders each decorated window with one SceneFX `wlr_scene_border` node.
The node owns both color bands and submits one draw through the dedicated
`border.frag` shader. The regular rounded-rectangle shader is not part of the
window border path.

## Geometry contract

The final decorated outline is authoritative. A border node receives:

- the content box and explicit inner, seam, and outer corner radii;
- the inner and outer logical widths;
- the inner and outer premultiplied colors;
- a raster box large enough for the complete stroke and antialias coverage.

`makeBorderRing` adds one transparent logical pixel outside the configured total
width. This margin does not affect layout geometry or the visible border width.
It only ensures that a fractional outer sample is not clipped by the scene-node
quad before the fragment shader can evaluate it.

Scene rendering scales widths and the already-derived contour radii from logical
coordinates. The shader therefore receives physical geometry consistently, such
as a 1.25-pixel stroke on an output with 1.25 scale.

## Fragment contract

`border.frag` evaluates independent signed distances for three nested rounded
rectangles. Let the smooth nested radius at inset $d$ be:

$$
N(R, d) =
\begin{cases}
0 & R = 0 \\
\max\left(1, \operatorname{round}\left(\frac{R^2}{R + d}\right)\right) & R > 0
\end{cases}
$$

For configured outer radius $R$, inner width $I$, and outer width $O$, the
contours are:

1. outer edge: content box outset by $I + O$, radius $R$;
2. color seam: content box outset by $I$, radius $N(R, O)$;
3. content edge: content box, radius $N(R, I + O)$.

The rational curve equals $R$ at zero inset and remains positive for every
positive configured radius. It trades strictly constant corner thickness for
inner contours that decrease smoothly instead of abruptly becoming square.

Each contour uses the same Euclidean rounded-rectangle distance. Straight edges
and rounded corners remain continuous at their tangency. The two colors mix at
their shared boundary inside one fragment; they are never overlapping
transparent scene nodes.

Inner and outer edge coverage is symmetric around the corresponding geometric
boundary. A zero outer width selects the inner color for the complete stroke; a
zero inner width selects the outer color.

## CPU clipping

SceneFX limits fragment work by subtracting areas that are certainly inside the
transparent content hole. Rounded corners must remain shader-owned.
`apply_clip_region` uses the explicit content radii and subtracts only a central
horizontal and vertical cross. Integer truncation of a diagonal approximation
can otherwise remove an isolated fragment before the shader runs.

## Scene lifecycle

View decorations, overview cards, and their close-animation snapshots all copy
or animate one `wlr_scene_border`. Focus animation updates the inner color;
opacity animation updates both premultiplied colors. The outer color never needs
a second scene node or draw order.

## Regression coverage

- `365_fractional_border_coverage.sh` checks a one-logical-pixel border at scale
  1.25. The top and side must have equal opaque and fractional coverage.
- `722_subsurface_border_corner.sh` checks the outer arc, smooth two-color seam,
  positive content radius, and straight-to-curve tangency against a full-window
  subsurface.
- `723_small_border_corner.sh` verifies that a one-pixel outer radius does not
  grow with a thick double border, an eight-pixel radius keeps its inner contour
  rounded, and zero preserves a square outer corner.
- `border-ring` unit tests protect the transparent raster margin and content-hole
  geometry.

When changing border geometry or antialiasing, temporarily break the relevant
invariant and confirm its regression check fails at the intended sample.
