#pragma once

extern "C" {
#include <scenefx/types/fx/clipped_region.h>
#include <wlr/util/box.h>
}

namespace umbriel {
  [[nodiscard]] constexpr wlr_box surfaceClipForOutput(
      const wlr_box& geometry, const wlr_box& content, const wlr_box& visible, int fullscreenOffsetX,
      int fullscreenOffsetY
  ) {
    return {
        .x = geometry.x + visible.x - content.x - fullscreenOffsetX,
        .y = geometry.y + visible.y - content.y - fullscreenOffsetY,
        .width = visible.width,
        .height = visible.height,
    };
  }

  // Scene clipping resizes the rendered buffer to its visible part. Remove the radius from every cut edge so that the
  // new boundary reads as a clip, not as a smaller rounded window.
  [[nodiscard]] constexpr fx_corner_radii
  cornerRadiiForVisible(const wlr_box& full, const wlr_box& visible, fx_corner_radii corners) {
    const bool left = visible.x > full.x;
    const bool right = visible.x + visible.width < full.x + full.width;
    const bool top = visible.y > full.y;
    const bool bottom = visible.y + visible.height < full.y + full.height;
    return corner_radii_new(
        left || top ? 0 : corners.top_left, right || top ? 0 : corners.top_right,
        right || bottom ? 0 : corners.bottom_right, left || bottom ? 0 : corners.bottom_left
    );
  }

  // Offset centering a client buffer of `contentSize` in a fullscreen tile of `tileSize`. Negative when the buffer is
  // the larger of the two, which crops it equally on both sides; a fullscreen buffer is never scaled, because scaling a
  // client that is mid mode-change shows it at the wrong aspect ratio.
  [[nodiscard]] constexpr int fullscreenCenterOffset(int tileSize, int contentSize) {
    return contentSize > 0 ? (tileSize - contentSize) / 2 : 0;
  }

  // Map the visible part of an animated presentation back onto the committed buffer. `content` is the box the view is
  // being drawn at and `clip` the visible part of it (both surface coordinates); `base` is the surface's own buffer
  // source box, which carries any viewport and scale the client set. This exists because a scene clip cannot express
  // it: a clip crops 1:1 and caps the destination at the committed surface size, so it cannot show a buffer at a size
  // the client has not committed. Returns an empty box when the clamp leaves nothing, meaning the buffer should be left
  // alone.
  [[nodiscard]] constexpr wlr_fbox croppedSourceBox(
      const wlr_fbox& base, const wlr_box& geometry, const wlr_box& content, const wlr_box& clip, int surfaceWidth,
      int surfaceHeight
  ) {
    if (content.width <= 0
        || content.height <= 0
        || geometry.width <= 0
        || geometry.height <= 0
        || surfaceWidth <= 0
        || surfaceHeight <= 0) {
      return {};
    }
    // Surface px per presented px.
    const double fx = static_cast<double>(geometry.width) / content.width;
    const double fy = static_cast<double>(geometry.height) / content.height;
    // Surface-local region backing the visible presented box.
    const double sx = geometry.x + (clip.x - geometry.x) * fx;
    const double sy = geometry.y + (clip.y - geometry.y) * fy;
    const double sw = clip.width * fx;
    const double sh = clip.height * fy;
    // Surface -> buffer coordinates (viewport/scale aware).
    const double bx = base.width / surfaceWidth;
    const double by = base.height / surfaceHeight;

    wlr_fbox src{base.x + sx * bx, base.y + sy * by, sw * bx, sh * by};
    if (src.x < base.x) {
      src.width -= base.x - src.x;
      src.x = base.x;
    }
    if (src.y < base.y) {
      src.height -= base.y - src.y;
      src.y = base.y;
    }
    const double maxWidth = base.x + base.width - src.x;
    const double maxHeight = base.y + base.height - src.y;
    src.width = src.width < maxWidth ? src.width : maxWidth;
    src.height = src.height < maxHeight ? src.height : maxHeight;
    if (src.width <= 0 || src.height <= 0) {
      return {};
    }
    return src;
  }

  [[nodiscard]] constexpr wlr_box intersect(const wlr_box& a, const wlr_box& b) {
    const int x = a.x > b.x ? a.x : b.x;
    const int y = a.y > b.y ? a.y : b.y;
    const int right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    const int bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    return {.x = x, .y = y, .width = right - x, .height = bottom - y};
  }

  // The region of a view's node that blur is allowed to sample, in coordinates local to the node. `target` is the
  // node's origin in layout space. SceneFX samples past the node's own bounds by roughly radius*passes pixels, so a
  // view sitting flush against an output edge would pull in whatever is on the neighbouring output. Insetting by that
  // bleed on any edge that touches the output boundary keeps the sample inside. Returns an empty box (width or height
  // <= 0) when nothing may be blurred and the effect must be hidden.
  [[nodiscard]] constexpr wlr_box blurClipForOutput(
      const wlr_box& nodeBox, const wlr_box& contentVisible, const wlr_box& outputBox, const wlr_box& target, int bleed
  ) {
    const wlr_box outputLocal{
        .x = outputBox.x - target.x,
        .y = outputBox.y - target.y,
        .width = outputBox.width,
        .height = outputBox.height,
    };
    const wlr_box contentLocal{
        .x = contentVisible.x - target.x,
        .y = contentVisible.y - target.y,
        .width = contentVisible.width,
        .height = contentVisible.height,
    };

    wlr_box clip = intersect(nodeBox, contentLocal);
    if (clip.width <= 0 || clip.height <= 0) {
      return {};
    }
    clip = intersect(clip, outputLocal);
    if (clip.width <= 0 || clip.height <= 0) {
      return {};
    }

    if (bleed > 0) {
      if (clip.x <= outputLocal.x) {
        clip.x += bleed;
        clip.width -= bleed;
      }
      if (clip.y <= outputLocal.y) {
        clip.y += bleed;
        clip.height -= bleed;
      }
      if (clip.x + clip.width >= outputLocal.x + outputLocal.width) {
        clip.width -= bleed;
      }
      if (clip.y + clip.height >= outputLocal.y + outputLocal.height) {
        clip.height -= bleed;
      }
    }
    if (clip.width <= 0 || clip.height <= 0) {
      return {};
    }
    return clip;
  }
} // namespace umbriel
