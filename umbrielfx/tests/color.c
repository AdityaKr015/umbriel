#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <xf86drm.h>

#include "render/color.h"
#include "render/fx_renderer/fx_renderer.h"
#include "scenefx/render/fx_renderer/fx_renderer.h"
#include "scenefx/render/fx_renderer/fx_offscreen_buffers.h"
#include "scenefx/render/pass.h"
#include "scenefx/types/fx/blur_data.h"

#define TEST_WIDTH 16
#define TEST_HEIGHT 16

struct fixture {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output *output;
	int drm_fd;
};

static bool check(bool condition, const char *message) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
	}
	return condition;
}

static bool fixture_try_device(struct fixture *fixture, const char *path) {
	int drm_fd = open(path, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		return false;
	}

	struct wlr_renderer *renderer = fx_renderer_create_with_drm_fd(drm_fd);
	if (renderer == NULL || !renderer->features.output_color_transform) {
		if (renderer != NULL) {
			wlr_renderer_destroy(renderer);
		}
		close(drm_fd);
		return false;
	}

	fixture->backend->buffer_caps |= WLR_BUFFER_CAP_DMABUF;
	struct wlr_allocator *allocator =
		wlr_allocator_autocreate(fixture->backend, renderer);
	if (allocator == NULL) {
		wlr_renderer_destroy(renderer);
		close(drm_fd);
		return false;
	}

	fixture->renderer = renderer;
	fixture->allocator = allocator;
	fixture->drm_fd = drm_fd;
	return true;
}

static bool fixture_init(struct fixture *fixture) {
	*fixture = (struct fixture) { .drm_fd = -1 };
	fixture->display = wl_display_create();
	if (fixture->display == NULL) {
		return false;
	}
	fixture->backend = wlr_headless_backend_create(
		wl_display_get_event_loop(fixture->display));
	if (fixture->backend == NULL) {
		return false;
	}

	const char *requested_device = getenv("SCENEFX_TEST_DRM_DEVICE");
	if (requested_device != NULL &&
			fixture_try_device(fixture, requested_device)) {
		goto create_output;
	}

	drmDevicePtr devices[64] = {0};
	int devices_len = drmGetDevices2(0, devices, 64);
	for (int i = 0; i < devices_len && fixture->renderer == NULL; i++) {
		if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) {
			continue;
		}
		fixture_try_device(fixture, devices[i]->nodes[DRM_NODE_RENDER]);
	}
	if (devices_len > 0) {
		drmFreeDevices(devices, devices_len);
	}
	if (fixture->renderer == NULL) {
		return false;
	}

create_output:
	fixture->output = wlr_headless_add_output(
		fixture->backend, TEST_WIDTH, TEST_HEIGHT);
	if (fixture->output == NULL || !wlr_output_init_render(fixture->output,
			fixture->allocator, fixture->renderer)) {
		return false;
	}
	fx_renderer_set_allocator(fixture->renderer, fixture->allocator);
	return true;
}

static void fixture_finish(struct fixture *fixture) {
	if (fixture->allocator != NULL) {
		wlr_allocator_destroy(fixture->allocator);
	}
	if (fixture->renderer != NULL) {
		wlr_renderer_destroy(fixture->renderer);
	}
	if (fixture->backend != NULL) {
		wlr_backend_destroy(fixture->backend);
	}
	if (fixture->display != NULL) {
		wl_display_destroy(fixture->display);
	}
	if (fixture->drm_fd >= 0) {
		close(fixture->drm_fd);
	}
}

static const struct wlr_drm_format *get_render_format(
		struct fixture *fixture, uint32_t format) {
	const struct wlr_drm_format_set *formats =
		fixture->renderer->impl->get_render_formats(fixture->renderer);
	return wlr_drm_format_set_get(formats, format);
}

static struct wlr_buffer *create_output_buffer(struct fixture *fixture,
		uint32_t format, int width, int height) {
	const struct wlr_drm_format *drm_format =
		get_render_format(fixture, format);
	if (drm_format == NULL) {
		return NULL;
	}
	return wlr_allocator_create_buffer(
		fixture->allocator, width, height, drm_format);
}

static bool read_buffer(struct fixture *fixture, struct wlr_buffer *buffer,
		uint32_t format, uint32_t stride, void *data) {
	struct wlr_texture *texture =
		wlr_texture_from_buffer(fixture->renderer, buffer);
	if (texture == NULL) {
		return false;
	}
	bool ok = wlr_texture_read_pixels(texture,
		&(struct wlr_texture_read_pixels_options) {
			.data = data,
			.format = format,
			.stride = stride,
		});
	wlr_texture_destroy(texture);
	return ok;
}

static bool render_texture(struct fixture *fixture, int width, int height,
		uint32_t output_format, uint32_t input_format, uint32_t input_stride,
		const void *input_data, enum wlr_color_transfer_function input_tf,
		const struct wlr_color_primaries *input_primaries,
		const float *luminance_multiplier,
		struct wlr_color_transform *output_transform,
		struct blur_data *blur_data, bool save_restore,
		uint32_t read_format, uint32_t read_stride, void *read_data) {
	struct wlr_texture *texture = wlr_texture_from_pixels(fixture->renderer,
		input_format, input_stride, 1, 1, input_data);
	if (!check(texture != NULL, "create input texture")) {
		return false;
	}
	struct wlr_buffer *output = create_output_buffer(
		fixture, output_format, width, height);
	if (!check(output != NULL, "allocate output buffer")) {
		wlr_texture_destroy(texture);
		return false;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		fixture->renderer, output, &(struct wlr_buffer_pass_options) {
			.color_transform = output_transform,
		});
	if (!check(pass != NULL, "begin render pass")) {
		wlr_buffer_drop(output);
		wlr_texture_destroy(texture);
		return false;
	}

	struct wlr_box box = { .width = width, .height = height };
	pixman_region32_t region;
	pixman_region32_init_rect(&region, 0, 0, width, height);
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options) {
		.box = box,
		.color = { .a = 1.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options) {
		.texture = texture,
		.dst_box = box,
		.clip = &region,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.filter_mode = WLR_SCALE_FILTER_NEAREST,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		.transfer_function = input_tf,
		.primaries = input_primaries,
		.luminance_multiplier = luminance_multiplier,
	});

	bool ok = true;
	if (blur_data != NULL) {
		ok = check(fx_render_pass_init_offscreen_buffers(
			pass, fixture->output), "initialize FP16 effect buffers");
		if (ok) {
			const float opacity = 1.0f;
			struct fx_render_blur_pass_options blur_options = {
				.tex_options = {
					.base = {
						.dst_box = box,
						.clip = &region,
						.transform = WL_OUTPUT_TRANSFORM_NORMAL,
						.filter_mode = WLR_SCALE_FILTER_BILINEAR,
						.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
						.alpha = &opacity,
					},
					.clip_box = &box,
				},
				.blur_data = blur_data,
				.blur_strength = 1.0f,
			};
			fx_render_pass_add_blur(fx_get_render_pass(pass), &blur_options);
		}
	}
	if (ok && save_restore) {
		// Mirror the blur-artifact compensation in wlr_scene_output_build_state:
		// the padding pixels are copied out of the pass target before the scene
		// renders and copied back afterwards.
		struct fx_gles_render_pass *fx_pass = fx_get_render_pass(pass);
		ok = check(fx_render_pass_init_offscreen_buffers(
			pass, fixture->output), "initialize save/restore buffers");
		if (ok) {
			fx_render_pass_read_to_buffer(fx_pass, &region,
				fx_pass->fx_offscreen_buffers->blur_saved_pixels_buffer,
				fx_pass->buffer);
			fx_render_pass_read_to_buffer(fx_pass, &region, fx_pass->buffer,
				fx_pass->fx_offscreen_buffers->blur_saved_pixels_buffer);
		}
	}
	pixman_region32_fini(&region);
	ok = wlr_render_pass_submit(pass) && ok;
	wlr_texture_destroy(texture);
	if (ok) {
		ok = check(read_buffer(fixture, output, read_format,
			read_stride, read_data), "read output buffer");
	}
	wlr_buffer_drop(output);
	return ok;
}

static float srgb_to_linear(float value) {
	return value <= 0.04045f ? value / 12.92f :
		powf((value + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float value) {
	return value <= 0.0031308f ? value * 12.92f :
		1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

static float linear_to_gamma22(float value) {
	return powf(fmaxf(value, 0.0f), 1.0f / 2.2f);
}

static float linear_to_pq(float value) {
	const float c1 = 0.8359375f;
	const float c2 = 18.8515625f;
	const float c3 = 18.6875f;
	const float m = 78.84375f;
	const float n = 0.1593017578125f;
	float powered = powf(fminf(fmaxf(value, 0.0f), 1.0f), n);
	return powf((c1 + c2 * powered) / (1.0f + c3 * powered), m);
}

static uint8_t to_u8(float value) {
	return lroundf(fminf(fmaxf(value, 0.0f), 1.0f) * 255.0f);
}

static uint16_t float_to_half(float value) {
	union {
		float value;
		uint32_t bits;
	} input = { .value = value };
	uint32_t sign = (input.bits >> 16) & 0x8000;
	int exponent = ((input.bits >> 23) & 0xFF) - 127 + 15;
	uint32_t mantissa = input.bits & 0x7FFFFF;
	if (exponent <= 0) {
		return sign;
	}
	if (exponent >= 31) {
		return sign | 0x7C00;
	}
	return sign | (exponent << 10) | (mantissa >> 13);
}

static struct wlr_color_transform *create_output_transform(
		enum wlr_color_named_primaries primaries,
		enum wlr_color_transfer_function tf, float scale) {
	struct wlr_color_primaries srgb;
	struct wlr_color_primaries target;
	wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	wlr_color_primaries_from_named(&target, primaries);
	float matrix[9];
	wlr_color_primaries_transform_absolute_colorimetric(&srgb, &target, matrix);
	for (size_t i = 0; i < 9; i++) {
		matrix[i] *= scale;
	}
	struct wlr_color_transform *matrix_transform =
		wlr_color_transform_init_matrix(matrix);
	struct wlr_color_transform *eotf_transform =
		wlr_color_transform_init_linear_to_inverse_eotf(tf);
	if (matrix_transform == NULL || eotf_transform == NULL) {
		wlr_color_transform_unref(matrix_transform);
		wlr_color_transform_unref(eotf_transform);
		return NULL;
	}
	struct wlr_color_transform *parts[] = {
		matrix_transform,
		eotf_transform,
	};
	struct wlr_color_transform *pipeline =
		wlr_color_transform_init_pipeline(parts, 2);
	wlr_color_transform_unref(matrix_transform);
	wlr_color_transform_unref(eotf_transform);
	return pipeline;
}

static bool codes_close(const uint8_t actual[static 4],
		const uint8_t expected[static 4], int tolerance) {
	for (size_t i = 0; i < 4; i++) {
		if (abs((int)actual[i] - expected[i]) > tolerance) {
			fprintf(stderr, "FAIL: channel %zu expected %u, got %u\n",
				i, expected[i], actual[i]);
			return false;
		}
	}
	return true;
}

static bool test_legacy_gamma22_target(struct fixture *fixture) {
	const uint8_t input[4] = { 26, 64, 128, 255 };
	uint8_t implicit_output[4] = {0};
	uint8_t explicit_output[4] = {0};
	struct wlr_color_primaries srgb;
	wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	struct wlr_color_transform *transform =
		wlr_color_transform_init_linear_to_inverse_eotf(
			WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
	bool ok = check(transform != NULL, "create Gamma 2.2 output transform") &&
		render_texture(fixture, 1, 1, DRM_FORMAT_ABGR8888,
			DRM_FORMAT_ABGR8888, 4, input,
			WLR_COLOR_TRANSFER_FUNCTION_GAMMA22, &srgb,
			NULL, NULL, NULL, false, DRM_FORMAT_ABGR8888, 4,
			implicit_output) &&
		codes_close(implicit_output, input, 0) &&
		render_texture(fixture, 1, 1, DRM_FORMAT_ABGR8888,
			DRM_FORMAT_ABGR8888, 4, input,
			WLR_COLOR_TRANSFER_FUNCTION_GAMMA22, &srgb,
			NULL, transform, NULL, false, DRM_FORMAT_ABGR8888, 4,
			explicit_output) &&
		codes_close(explicit_output, implicit_output, 1);
	wlr_color_transform_unref(transform);
	return ok;
}

static bool test_srgb_to_gamma22(struct fixture *fixture) {
	const uint8_t input[4] = { 26, 64, 128, 255 };
	uint8_t expected[4] = {0};
	for (size_t i = 0; i < 3; i++) {
		expected[i] = to_u8(linear_to_gamma22(
			srgb_to_linear((float)input[i] / 255.0f)));
	}
	expected[3] = 255;

	uint8_t output[4] = {0};
	struct wlr_color_primaries srgb;
	wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	return render_texture(fixture, 1, 1, DRM_FORMAT_ABGR8888,
		DRM_FORMAT_ABGR8888, 4, input, WLR_COLOR_TRANSFER_FUNCTION_SRGB,
		&srgb, NULL, NULL, NULL, false, DRM_FORMAT_ABGR8888, 4, output) &&
		codes_close(output, expected, 1);
}

static bool test_pq_roundtrip(struct fixture *fixture) {
	const uint8_t input[4] = { 96, 144, 208, 255 };
	uint8_t output[4] = {0};
	struct wlr_color_primaries bt2020;
	wlr_color_primaries_from_named(
		&bt2020, WLR_COLOR_NAMED_PRIMARIES_BT2020);
	const float input_luminance = 10000.0f / 203.0f;
	struct wlr_color_transform *transform = create_output_transform(
		WLR_COLOR_NAMED_PRIMARIES_BT2020,
		WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f);
	bool ok = check(transform != NULL, "create PQ output transform") &&
		render_texture(fixture, 1, 1, DRM_FORMAT_ABGR8888,
			DRM_FORMAT_ABGR8888, 4, input,
			WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, &bt2020,
			&input_luminance, transform, NULL, false,
			DRM_FORMAT_ABGR8888, 4, output) &&
		codes_close(output, input, 1);
	wlr_color_transform_unref(transform);
	return ok;
}

static bool test_bt2020_to_srgb(struct fixture *fixture) {
	const uint8_t input[4] = { 156, 132, 104, 255 };
	uint8_t output[4] = {0};
	struct wlr_color_primaries srgb;
	struct wlr_color_primaries bt2020;
	wlr_color_primaries_from_named(&srgb, WLR_COLOR_NAMED_PRIMARIES_SRGB);
	wlr_color_primaries_from_named(
		&bt2020, WLR_COLOR_NAMED_PRIMARIES_BT2020);
	float matrix[9];
	wlr_color_primaries_transform_absolute_colorimetric(
		&bt2020, &srgb, matrix);
	float linear[3] = {
		srgb_to_linear((float)input[0] / 255.0f),
		srgb_to_linear((float)input[1] / 255.0f),
		srgb_to_linear((float)input[2] / 255.0f),
	};
	uint8_t expected[4] = {0};
	for (size_t row = 0; row < 3; row++) {
		float value = matrix[row * 3] * linear[0] +
			matrix[row * 3 + 1] * linear[1] +
			matrix[row * 3 + 2] * linear[2];
		expected[row] = to_u8(linear_to_srgb(fmaxf(value, 0.0f)));
	}
	expected[3] = 255;
	struct wlr_color_transform *transform =
		wlr_color_transform_init_linear_to_inverse_eotf(
			WLR_COLOR_TRANSFER_FUNCTION_SRGB);
	bool ok = check(transform != NULL, "create sRGB output transform") &&
		render_texture(fixture, 1, 1, DRM_FORMAT_ABGR8888,
			DRM_FORMAT_ABGR8888, 4, input,
			WLR_COLOR_TRANSFER_FUNCTION_SRGB, &bt2020, NULL,
			transform, NULL, false, DRM_FORMAT_ABGR8888, 4, output) &&
		codes_close(output, expected, 1);
	wlr_color_transform_unref(transform);
	return ok;
}

static uint32_t select_10bit_format(struct fixture *fixture) {
	if (get_render_format(fixture, DRM_FORMAT_ABGR2101010) != NULL) {
		return DRM_FORMAT_ABGR2101010;
	}
	if (get_render_format(fixture, DRM_FORMAT_XBGR2101010) != NULL) {
		return DRM_FORMAT_XBGR2101010;
	}
	return DRM_FORMAT_INVALID;
}

static bool test_fp16(struct fixture *fixture, bool blur) {
	uint32_t format = select_10bit_format(fixture);
	if (!check(format != DRM_FORMAT_INVALID, "find 10-bit output format")) {
		return false;
	}
	const uint16_t input[4] = {
		float_to_half(1.5f),
		float_to_half(1.5f),
		float_to_half(1.5f),
		float_to_half(0.5f),
	};
	uint32_t output[TEST_WIDTH * TEST_HEIGHT] = {0};
	struct wlr_color_transform *transform = create_output_transform(
		WLR_COLOR_NAMED_PRIMARIES_SRGB,
		WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f);
	struct blur_data blur_options = {
		.num_passes = 1,
		.radius = 1.0f,
		.brightness = 1.0f,
		.contrast = 1.0f,
		.saturation = 1.0f,
	};
	bool ok = check(transform != NULL, "create FP16 output transform") &&
		render_texture(fixture, TEST_WIDTH, TEST_HEIGHT, format,
			DRM_FORMAT_ABGR16161616F, sizeof(input), input,
			WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, NULL, NULL,
			transform, blur ? &blur_options : NULL, false, format,
			TEST_WIDTH * sizeof(uint32_t), output);
	wlr_color_transform_unref(transform);
	if (!ok) {
		return false;
	}

	uint32_t pixel = output[(TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2];
	int actual = pixel & 0x3FF;
	int expected = lroundf(linear_to_pq(1.5f * 203.0f / 10000.0f) * 1023.0f);
	int sdr_white = lroundf(linear_to_pq(203.0f / 10000.0f) * 1023.0f);
	int tolerance = blur ? 6 : 2;
	return check(abs(actual - expected) <= tolerance,
		blur ? "blur preserves FP16 luminance" : "FP16 blend preserves luminance") &&
		check(actual > sdr_white + 2, "value remains above SDR white");
}

static bool test_fp16_blur_effects(struct fixture *fixture) {
	uint32_t format = select_10bit_format(fixture);
	if (!check(format != DRM_FORMAT_INVALID, "find 10-bit output format")) {
		return false;
	}
	const float input_linear = 0.05f;
	const uint16_t input[4] = {
		float_to_half(input_linear),
		float_to_half(input_linear),
		float_to_half(input_linear),
		float_to_half(1.0f),
	};
	uint32_t output[TEST_WIDTH * TEST_HEIGHT] = {0};
	struct wlr_color_transform *transform = create_output_transform(
		WLR_COLOR_NAMED_PRIMARIES_SRGB,
		WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f);
	struct blur_data blur_options = {
		.num_passes = 1,
		.radius = 1.0f,
		.brightness = 0.9f,
		.contrast = 0.9f,
		.saturation = 1.1f,
	};
	bool ok = check(transform != NULL, "create FP16 blur-effects output transform") &&
		render_texture(fixture, TEST_WIDTH, TEST_HEIGHT, format,
			DRM_FORMAT_ABGR16161616F, sizeof(input), input,
			WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, NULL, NULL,
			transform, &blur_options, false, format,
			TEST_WIDTH * sizeof(uint32_t), output);
	wlr_color_transform_unref(transform);
	if (!ok) {
		return false;
	}

	float encoded = linear_to_srgb(input_linear);
	encoded = (encoded * 0.9f + 0.05f) - 0.1f;
	float expected_linear = srgb_to_linear(encoded);
	int expected = lroundf(linear_to_pq(
		expected_linear * 203.0f / 10000.0f) * 1023.0f);
	uint32_t pixel = output[(TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2];
	int actual = pixel & 0x3FF;
	return check(abs(actual - expected) <= 6,
		"FP16 blur effects preserve gamma-space brightness");
}

// The blur-artifact compensation copies the pass target into a scratch buffer
// and back. In two-pass mode the pass target holds linear light, so the copy
// must be lossless: any transfer-function conversion darkens the padding
// region around every blur node.
static bool test_fp16_save_restore(struct fixture *fixture) {
	uint32_t format = select_10bit_format(fixture);
	if (!check(format != DRM_FORMAT_INVALID, "find 10-bit output format")) {
		return false;
	}
	const float input_linear = 0.2f;
	const uint16_t input[4] = {
		float_to_half(input_linear),
		float_to_half(input_linear),
		float_to_half(input_linear),
		float_to_half(1.0f),
	};
	uint32_t output[TEST_WIDTH * TEST_HEIGHT] = {0};
	struct wlr_color_transform *transform = create_output_transform(
		WLR_COLOR_NAMED_PRIMARIES_SRGB,
		WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ, 203.0f / 10000.0f);
	bool ok = check(transform != NULL, "create FP16 save/restore output transform") &&
		render_texture(fixture, TEST_WIDTH, TEST_HEIGHT, format,
			DRM_FORMAT_ABGR16161616F, sizeof(input), input,
			WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR, NULL, NULL,
			transform, NULL, true, format,
			TEST_WIDTH * sizeof(uint32_t), output);
	wlr_color_transform_unref(transform);
	if (!ok) {
		return false;
	}

	int expected = lroundf(linear_to_pq(
		input_linear * 203.0f / 10000.0f) * 1023.0f);
	uint32_t pixel = output[(TEST_HEIGHT / 2) * TEST_WIDTH + TEST_WIDTH / 2];
	int actual = pixel & 0x3FF;
	return check(abs(actual - expected) <= 2,
		"FP16 save/restore round trip preserves luminance");
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s CASE\n", argv[0]);
		return EXIT_FAILURE;
	}

	struct fixture fixture;
	if (!fixture_init(&fixture)) {
		fprintf(stderr, "SKIP: no FP16-capable DRM render node\n");
		fixture_finish(&fixture);
		return 77;
	}

	bool ok;
	if (strcmp(argv[1], "legacy-gamma22-target") == 0) {
		ok = test_legacy_gamma22_target(&fixture);
	} else if (strcmp(argv[1], "srgb-to-gamma22") == 0) {
		ok = test_srgb_to_gamma22(&fixture);
	} else if (strcmp(argv[1], "pq-roundtrip") == 0) {
		ok = test_pq_roundtrip(&fixture);
	} else if (strcmp(argv[1], "bt2020-to-srgb") == 0) {
		ok = test_bt2020_to_srgb(&fixture);
	} else if (strcmp(argv[1], "fp16-blending") == 0) {
		ok = test_fp16(&fixture, false);
	} else if (strcmp(argv[1], "fp16-blur") == 0) {
		ok = test_fp16(&fixture, true);
	} else if (strcmp(argv[1], "fp16-blur-effects") == 0) {
		ok = test_fp16_blur_effects(&fixture);
	} else if (strcmp(argv[1], "fp16-save-restore") == 0) {
		ok = test_fp16_save_restore(&fixture);
	} else {
		fprintf(stderr, "unknown case: %s\n", argv[1]);
		ok = false;
	}

	fixture_finish(&fixture);
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
