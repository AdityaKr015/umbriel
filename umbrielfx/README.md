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
| `tests/`                | Color transform regressions                        |

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
- Exports `wlr_scene_*`, `wlr_egl_*`, `wlr_color_*`, and `wlr_matrix_*` names
  that collide with `libwlroots`. Do not add another wlroots-shadowing symbol.

## License

MIT, see `LICENSE`. Copyright is held by the SceneFX and wlroots contributors
listed there, plus Umbriel contributors.
