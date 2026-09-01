# umbrielfx

Umbriel's scene graph and GLES2 renderer, a hard fork of
[wlrfx/scenefx](https://github.com/wlrfx/scenefx).

## Layout

| Path                    | Contents                                          |
| ----------------------- | ------------------------------------------------- |
| `include/umbrielfx/`    | Public API, the only headers the compositor sees   |
| `internal/`             | Private headers, not on the compositor's include path |
| `render/`               | EGL setup, color transforms, pixel formats        |
| `render/fx_renderer/`   | GLES2 renderer, render passes, shaders            |
| `types/`                | Scene graph, output helpers, blur and clip state   |
| `util/`                 | Helpers shared inside the library                  |
| `tests/`                | Color transform and scene ABI regressions          |

## Building

The root `meson.build` pulls this directory in with `subdir('umbrielfx')` and
links the resulting archive into the compositor. Its tests run in the
`umbrielfx` suite:

```sh
meson test -C build --suite umbrielfx
```

## Constraints

- Compiled against wlroots' private struct layouts (`-DWLR_PRIVATE=`), so it is
  pinned to one wlroots minor series.
- C, not C++. Its compiler flags stay on the `umbrielfx_lib` target and must not
  reach the compositor's C++23 translation units.
- Shaders are embedded as generated char arrays, so an installed binary never
  locates shader data files at runtime.
- Replaces wlroots' scene graph but reuses the scene helpers it does not
  reimplement (`wlr_scene_xdg_surface_create`, `wlr_scene_subsurface_tree_create`,
  `wlr_scene_layer_surface_v1_create`, `wlr_scene_drag_icon_create`,
  `wlr_scene_attach_output_layout`, `wlr_scene_output_layout_add_output`). Those
  resolve to `libwlroots` and read these structs at wlroots' field offsets, so
  every struct in `types/wlr_scene.h` that wlroots also declares must stay a
  strict prefix extension: fields Umbriel adds go after every wlroots field.
  `tests/abi.c` enforces this.

## License

MIT, see `LICENSE`. Copyright is held by the SceneFX and wlroots contributors
listed there, plus Umbriel contributors.
