#pragma once

struct wlr_box;
struct wlr_scene_blur;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  // Owns the desired-state logic for one SceneFX backdrop-blur node. The node
  // itself is a child of the owner's scene tree and is freed by scene-tree
  // teardown, never by this class (no destructor).
  class SurfaceBlur {
  public:
    // Creates/updates/disables the node to match config and surface state.
    // nodeBox: content box in `parent` coordinates (position + size of the node).
    // surfaceBox: content box in surface-local coordinates (opaque-region test).
    void update(
        wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox,
        int cornerRadius
    );
    // Disable the node (unmap path); update() re-enables.
    void hide();

  private:
    wlr_scene_blur* m_node = nullptr;
    bool m_masked = false;
  };

} // namespace umbriel
