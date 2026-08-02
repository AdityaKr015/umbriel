# Umbriel

A Wayland compositor built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.20 and
[SceneFX](https://github.com/wlrfx/scenefx).

Early stage. Right now the goal is a clean modular C++23 skeleton that launches and owns outputs.

## Status

What works today:

- Nestable / DRM backend startup via wlroots
- Output hotplug, modeset, and SceneFX-backed scene commits
- Clean shutdown on `SIGINT` / `SIGTERM`

Still open / planned:

| Area | Direction |
|------|-----------|
| Layouts | Scrolling (H/V), dwindle, master |
| Eyecandy | Blur, shadows, rounded corners, double borders |
| Workspaces | Tags vs workspaces, per monitor |
| Config | TOML with includes |
| Xwayland | Native vs satellite |
| Shell | Full Noctalia support without Noctalia logic changes |
| Overview | Undecided |
| Protocols | See roadmap below |

## Dependencies

- C++23 compiler
- meson, ninja, pkg-config, wayland-scanner
- wlroots 0.20
- scenefx 0.5
- wayland, libxkbcommon, libinput, pixman, libGL, libdrm

## Build (Nix)

```sh
nix develop
just debug          # debug build -> build-debug/umbriel
just release        # release build -> build-release/umbriel
just run            # build debug and run (nested session)
just lint
just clean
```

Or build the package:

```sh
nix build
./result/bin/umbriel
```

SceneFX comes in as a flake input (`github:wlrfx/scenefx/0.5`).

## Build (system packages)

If `wlroots-0.20` and `scenefx-0.5` are available via pkg-config:

```sh
just debug
./build-debug/umbriel
```

## Running

From an existing Wayland or X11 session, Umbriel opens a nested window. From a TTY it takes over the seat.

```sh
just run
# then in another terminal:
WAYLAND_DISPLAY=<socket from logs> weston-terminal
```

Stop with `Ctrl+C`.

## Project layout

```text
src/
  main.cpp
  server/     display, backend, renderer, scene
  output/     per-output lifecycle and frame commits
nix/
  package.nix
  devshell.nix
```

`src/` is the include root. Headers live next to their sources.

## Protocol roadmap

Target support (not implemented yet):

- `zxdg_output_manager_v1`
- `zwlr_layer_shell_v1`
- `ext_session_lock_manager_v1`
- `zwp_pointer_constraints_v1`
- `zwp_relative_pointer_manager_v1`
- `wp_cursor_shape_manager_v1`
- `zwlr_foreign_toplevel_manager_v1`
- `zwp_idle_inhibit_manager_v1`
- `zwlr_screencopy_manager_v1`
- `zwlr_export_dmabuf_manager_v1`
- `xdg_activation_v1`
- `ext_workspace_manager_v1`
