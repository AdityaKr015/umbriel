#pragma once

struct wlr_scene_shadow;
struct wlr_scene_tree;
struct wlr_box;

namespace umbriel {

  // Owns the desired-state logic for one SceneFX drop-shadow node. The node is a
  // child of the owner's scene tree and freed by scene-tree teardown (no destructor).
  class SurfaceShadow {
  public:
    // contentWidth/Height: toplevel geometry size. borderTotal: decoration ring width drawn outside the content (0 when
    // borders are disabled/hidden). cornerRadius: radius of the decoration's outer edge (0 = square). clampBox (layout
    // coordinates, optional): the node is shrunk so it never extends past this box; the punched hole shifts to
    // compensate. Data-side equivalent of an output clip, needs no SceneFX patch.
    void update(
        wlr_scene_tree* parent, int contentWidth, int contentHeight, int borderTotal, int cornerRadius,
        const wlr_box* clampBox
    );
    // Disable the node (unmap/fullscreen/off-output path); update() re-enables.
    void hide();
    // Forget the node pointer (caller is destroying the parent tree externally).
    void reset();
    // Set an opacity multiplier applied to the shadow color (for fade animations).
    void setAlpha(float alpha);

  private:
    wlr_scene_shadow* m_node = nullptr;
    float m_alpha = 1.0F;
  };

} // namespace umbriel
