# Umbriel

A Wayland compositor built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.20 and
[SceneFX](https://github.com/wlrfx/scenefx).

Early stage. Clean modular C++23 compositor that launches, accepts input, maps xdg-shell windows, and hosts layer-shell surfaces.

## Status

What works today:

- Nestable / DRM backend startup via wlroots + SceneFX renderer
- Output hotplug, modeset, and SceneFX-backed scene commits
- Seat, keyboard, pointer/cursor, xdg-shell toplevels and popups
- `zwlr_layer_shell_v1` (anchors, exclusive zones, keyboard interactivity)
- `zwlr_foreign_toplevel_manager_v1` (active window / task list for shell clients)
- `zxdg_output_manager_v1` (logical size/position for shell clients)
- `ext_session_lock_manager_v1` (session lock / lock screen)
- `wp_cursor_shape_manager_v1` (named cursor shapes from clients)
- `zwp_pointer_constraints_v1` / `zwp_relative_pointer_manager_v1` (pointer lock/confine)
- `zwp_idle_inhibit_manager_v1` (+ `ext_idle_notifier_v1` for idle timers)
- `zwlr_screencopy_manager_v1` / `zwlr_export_dmabuf_manager_v1` (screenshots / capture)
- `xdg_activation_v1` (token-based window activation / focus)
- Nested sessions use **Alt** as mod, native DRM uses **Super**
- Keybinds: mod+Escape quit, mod+Return terminal, mod+F1 cycle windows
- Clean shutdown on `SIGINT` / `SIGTERM` / mod+Escape

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
just asan           # AddressSanitizer build -> build-asan/umbriel
just release        # release build -> build-release/umbriel
just run debug      # build debug and run (nested session)
just run asan       # build ASan and run
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

From an existing Wayland or X11 session, Umbriel opens a nested window (mod = Alt).
From a TTY it takes over the seat (mod = Super).

```sh
TERMINAL=ghostty just run debug
TERMINAL=ghostty just run asan
# or:
TERMINAL=ghostty ./build-debug/umbriel -s ghostty
```

Debug/ASan builds log at debug to stderr and `$XDG_CACHE_HOME/umbriel/umbriel.log`
(fallback `~/.cache/umbriel/umbriel.log`).

Inside the session:

| Shortcut | Action |
|----------|--------|
| mod+Escape | Quit |
| mod+Return | Spawn `$TERMINAL` |
| mod+F1 | Cycle window focus |
| mod+1..9 | Switch workspace on focused monitor |
| mod+Shift+1..9 | Move focused window to workspace and follow |

Set `TERMINAL` to your terminal binary (`ghostty`, `kitty`, `alacritty`, ...).

Stop with mod+Escape or `Ctrl+C` from the parent terminal.

## Project layout

```text
src/
  main.cpp
  wlr.h
  server/     display, backend, scene, xdg/layer/lock wiring
  output/     per-output lifecycle and frame commits
  input/      seat, keyboard, cursor
  view/       xdg toplevels and popups
  layer/      layer-shell surfaces
  lock/       ext-session-lock surfaces
  workspace/  per-monitor workspaces (ext-workspace)
protocols/    vendored Wayland protocol XML
nix/
  package.nix
  devshell.nix
```

`src/` is the include root. Headers live next to their sources.

## Protocol roadmap

Target support:

- `zwlr_layer_shell_v1` (done)
- `zwlr_foreign_toplevel_manager_v1` (done)
- `zxdg_output_manager_v1` (done)
- `ext_session_lock_manager_v1` (done)
- `zwp_pointer_constraints_v1` (done)
- `zwp_relative_pointer_manager_v1` (done)
- `wp_cursor_shape_manager_v1` (done)
- `zwp_idle_inhibit_manager_v1` (done)
- `zwlr_screencopy_manager_v1` (done)
- `zwlr_export_dmabuf_manager_v1` (done)
- `xdg_activation_v1` (done)
- `ext_workspace_manager_v1` (done)
