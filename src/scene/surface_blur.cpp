#include "scene/surface_blur.h"

#include "config/config.h"
// clang-format off
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on
#include <pixman.h>

namespace umbriel {

  namespace {

    wlr_scene_buffer* findSurfaceBuffer(wlr_scene_node& root, wlr_surface* surface) {
      struct Lookup {
        wlr_surface* surface;
        wlr_scene_buffer* buffer = nullptr;
      } lookup{surface};

      wlr_scene_node_for_each_buffer(
          &root,
          [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
            auto* lookup = static_cast<Lookup*>(data);
            if (lookup->buffer != nullptr) {
              return;
            }
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface != nullptr && sceneSurface->surface == lookup->surface) {
              lookup->buffer = buffer;
            }
          },
          &lookup
      );
      return lookup.buffer;
    }

    bool isTransparent(wlr_surface* surface, const wlr_box& surfaceBox) {
      const pixman_box32_t box = {
          surfaceBox.x, surfaceBox.y, surfaceBox.x + surfaceBox.width, surfaceBox.y + surfaceBox.height
      };
      return pixman_region32_contains_rectangle(&surface->opaque_region, &box) != PIXMAN_REGION_IN;
    }

  } // namespace

  void SurfaceBlur::update(
      wlr_scene_tree* parent, wlr_surface* surface, const wlr_box& nodeBox, const wlr_box& surfaceBox, int cornerRadius
  ) {
    const Config::Appearance::Blur& cfg = config().appearance.blur;
    const bool want = cfg.enabled && nodeBox.width > 0 && nodeBox.height > 0 && isTransparent(surface, surfaceBox);
    if (!want) {
      if (m_node != nullptr) {
        wlr_scene_node_set_enabled(&m_node->node, false);
      }
      return;
    }

    if (m_node != nullptr && m_masked != (cfg.ignoreAlpha > 0.0)) {
      wlr_scene_node_destroy(&m_node->node);
      m_node = nullptr;
    }

    if (m_node == nullptr) {
      m_node = wlr_scene_blur_create(parent, nodeBox.width, nodeBox.height);
      if (m_node == nullptr) {
        return;
      }
      wlr_scene_node_lower_to_bottom(&m_node->node);
      m_masked = cfg.ignoreAlpha > 0.0;
    }

    if (m_masked && wlr_scene_blur_get_transparency_mask_source(m_node) == nullptr) {
      if (wlr_scene_buffer* mask = findSurfaceBuffer(parent->node, surface)) {
        wlr_scene_blur_set_transparency_mask_source(m_node, mask);
      }
    }

    const float ignoreAlpha = static_cast<float>(cfg.ignoreAlpha);
    if (m_node->ignore_alpha != ignoreAlpha) {
      wlr_scene_blur_set_ignore_alpha(m_node, ignoreAlpha);
    }

    wlr_scene_node_set_enabled(&m_node->node, true);
    wlr_scene_node_set_position(&m_node->node, nodeBox.x, nodeBox.y);
    if (m_node->width != nodeBox.width || m_node->height != nodeBox.height) {
      wlr_scene_blur_set_size(m_node, nodeBox.width, nodeBox.height);
    }
    if (m_node->corners.top_left != cornerRadius) {
      wlr_scene_blur_set_corner_radius(m_node, cornerRadius);
    }
  }

  void SurfaceBlur::hide() {
    if (m_node != nullptr) {
      wlr_scene_node_set_enabled(&m_node->node, false);
    }
  }

} // namespace umbriel
