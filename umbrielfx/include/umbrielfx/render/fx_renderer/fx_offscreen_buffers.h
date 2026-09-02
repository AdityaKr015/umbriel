#ifndef _FX_OFFSCREEN_BUFFERS_H
#define _FX_OFFSCREEN_BUFFERS_H

#include <wlr/types/wlr_output.h>
#include <wlr/util/addon.h>

/**
 * Per-output effect framebuffers. Each buffer is allocated the first time an
 * effect needs it and kept, sized to the output, until the output goes away
 * or `fx_renderer_clear_output_effect_buffers` is called.
 */
struct fx_offscreen_buffers {
	struct wl_list link; // fx_renderer.offscreen_buffers
	struct wlr_addon addon;
	// Allocator of the output the buffers belong to
	struct wlr_allocator *allocator;

	// Contains the blurred background for tiled windows
	struct fx_framebuffer *optimized_blur_buffer;
	// Contains the non-blurred background for tiled windows. Used for blurring
	// optimized surfaces with an alpha. Just as inefficient as the regular blur.
	struct fx_framebuffer *optimized_no_blur_buffer;
	// Contains the original pixels to draw over the areas where artifact are visible
	struct fx_framebuffer *blur_saved_pixels_buffer;
	// Blur swaps between the two effects buffers every time it scales the image
	// Buffer used for effects
	struct fx_framebuffer *effects_buffer;
	// Swap buffer used for effects
	struct fx_framebuffer *effects_buffer_swapped;
};

void fx_offscreen_buffers_destroy(struct fx_offscreen_buffers *fbos);
struct fx_offscreen_buffers *fx_offscreen_buffers_try_get(struct wlr_output *output);

#endif
