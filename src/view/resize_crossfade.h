#pragma once

#include "core/animation.h"

#include <cstdint>
#include <vector>
#include <wayland-server-core.h>

struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_scene_tree;
struct wlr_surface;

namespace umbriel {

  class ColorManager;

  // The frame a layout-sized view showed before it committed its new size, faded out above the live buffers so the
  // content eases into the new size instead of switching in one frame. The frozen buffers are scaled into the presented
  // box exactly like the live ones, so both layers always share a size. The fade follows the presented size toward its
  // target, so it ends with the size animation; only a commit that lands after the size settled gets a timed fade.
  class ResizeCrossfade {
  public:
    ResizeCrossfade() = default;
    ~ResizeCrossfade();
    ResizeCrossfade(const ResizeCrossfade&) = delete;
    ResizeCrossfade& operator=(const ResizeCrossfade&) = delete;

    // Clone the enabled buffers of `root`'s surface tree under `viewTree`, stacked directly above `above` and hidden
    // until start(). They currently fill a `width` x `height` box at the view-tree origin. Replaces any earlier
    // capture.
    void capture(
        wlr_scene_tree* viewTree, wlr_scene_node* above, wlr_surface* root, int width, int height, int radius,
        ColorManager* colorManager
    );
    // Show the captured frame and fade it out as the presented size travels from `presentedWidth` x `presentedHeight`
    // to `targetWidth` x `targetHeight`, or over `durationMs` when it already arrived. Does nothing without a pending
    // capture.
    void start(
        int durationMs, const AnimationCurve& curve, int presentedWidth, int presentedHeight, int targetWidth,
        int targetHeight
    );
    // Advance the fade; true while it runs. The capture is released on the tick that completes it.
    bool tick(uint64_t nowMsec);
    // Scale the frozen buffers into the presented box; a size-following fade also takes its opacity from it.
    void present(int width, int height);
    // The view's own opacity, which the fade multiplies.
    void applyOpacity(float opacity);
    void discard();

    [[nodiscard]] bool pending() const { return m_tree != nullptr && !m_fading; }
    [[nodiscard]] bool active() const { return m_tree != nullptr && m_fading; }
    // The fade follows the presented size rather than its own clock.
    [[nodiscard]] bool followsSize() const { return active() && m_travel > 0; }

  private:
    struct Buffer {
      wlr_scene_buffer* node = nullptr;
      int x = 0;
      int y = 0;
      int width = 0;
      int height = 0;
    };

    static void onTreeDestroy(wl_listener* listener, void* data);
    void applyBufferOpacity();

    wlr_scene_tree* m_tree = nullptr;
    wl_listener m_treeDestroy{};
    std::vector<Buffer> m_buffers;
    int m_capturedWidth = 0;
    int m_capturedHeight = 0;
    int m_radius = 0;
    float m_opacity = 1.0F;
    AnimatedValue m_fade{1.0};
    bool m_fading = false;
    // Size-following fade: the target size and the distance left to it when the fade started; 0 for a timed fade.
    int m_targetWidth = 0;
    int m_targetHeight = 0;
    int m_travel = 0;
  };

} // namespace umbriel
