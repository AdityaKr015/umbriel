# Umbriel

A Wayland compositor built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.20 and
[SceneFX](https://github.com/wlrfx/scenefx).

Early stage. Clean modular C++23 compositor with a scrolling layout, workspaces, shell protocols, and TOML configuration.

## Status

What works today:

- Nestable / DRM backend startup via wlroots + SceneFX renderer
- Output hotplug, modeset, and SceneFX-backed scene commits
- Seat, keyboard, pointer/cursor, xdg-shell toplevels and popups
- Per-monitor workspaces via `ext_workspace_manager_v1` (9 workspaces each, isolated per output)
- Scrolling column layout with keyboard/mouse focus, movement, width presets, and animated transitions
- `zwlr_layer_shell_v1` (anchors, exclusive zones, keyboard interactivity)
- `zwlr_foreign_toplevel_manager_v1` (active window / task list for shell clients)
- `zxdg_output_manager_v1` (logical size/position for shell clients)
- `ext_session_lock_manager_v1` (session lock / lock screen)
- `wp_cursor_shape_manager_v1` (named cursor shapes from clients)
- `zwp_pointer_constraints_v1` / `zwp_relative_pointer_manager_v1` (pointer lock/confine)
- `zwp_idle_inhibit_manager_v1` (+ `ext_idle_notifier_v1` for idle timers)
- `zwlr_screencopy_manager_v1` / `zwlr_export_dmabuf_manager_v1` (screenshots / capture)
- `xdg_activation_v1` (token-based window activation / focus)
- `wp_viewporter` / `wp_fractional_scale_v1` (viewport crop + fractional scale; needed for Noctalia panel click shield)
- `ext_data_control_v1` (+ primary selection) for clipboard managers / history
- `zwlr_gamma_control_v1` (Noctalia night light / color temperature)
- Nested sessions use **Alt** as mod, native DRM uses **Super**
- Configurable keybinds with compiled defaults for focus/move, layout actions, applications, and workspaces
- Native DRM: Ctrl+Alt+F1..F12 switches VT
- Clean shutdown on `SIGINT` / `SIGTERM` / mod+Escape
- Noctalia shell runs against the protocols above

Still open / planned:

| Area | Direction |
|------|-----------|
| Layouts | Vertical scrolling, dwindle, master |
| Eyecandy | Blur, shadows, rounded corners, double borders |
| Xwayland | Native vs satellite |
| Shell | Remaining Noctalia polish (output management, IME, …) |
| Overview | Undecided |
| Protocols | See roadmap below |

## Dependencies

- C++23 compiler
- meson, ninja, pkg-config, wayland-scanner
- wlroots 0.20
- scenefx 0.5
- wayland, libxkbcommon, libinput, pixman, libGL, libdrm, tomlplusplus

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

SceneFX comes in as a flake input (`github:wlrfx/scenefx/0.5`). Prefer `nix develop` for local
builds so wlroots, SceneFX, and tomlplusplus are on `PKG_CONFIG_PATH`.

## Build (system packages)

If `wlroots-0.20`, `scenefx-0.5`, and `tomlplusplus` are available via pkg-config:

```sh
just debug
./build-debug/umbriel
```

Umbriel needs SceneFX `ignore_alpha`. The patch lives at
`subprojects/packagefiles/scenefx-blur-ignore-alpha.diff`. Meson uses system
`scenefx-0.5` only when that API is in the headers; otherwise it builds the wrap
from `subprojects/scenefx.wrap`, which applies the same diff. Nix packaging applies
the patch in `nix/package.nix`.
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
| mod+Return | Spawn the configured terminal |
| mod+F1 | Cycle window focus |
| mod+H/J/K/L or arrows | Focus adjacent window |
| mod+Shift+H/J/K/L or arrows | Move focused window |
| mod+comma / mod+period | Consume left / expel right |
| mod+R / mod+F | Cycle width / toggle full width |
| mod+1..9 | Switch workspace on focused monitor |
| mod+Shift+1..9 | Move focused window to workspace and follow |

Set `TERMINAL` to your terminal binary (`ghostty`, `kitty`, `alacritty`, ...).

Stop with mod+Escape or `Ctrl+C` from the parent terminal.

## Configuration

Umbriel loads `$XDG_CONFIG_HOME/umbriel/config.toml` (normally `~/.config/umbriel/config.toml`) at startup.
Pass `-c path/to/config.toml` to use another file. Config files can include other TOML files with
`[include] files = ["theme.toml", "keybinds.toml"]`; later files and the main file override earlier values.

See [`example.toml`](example.toml) for every available option, its default or supported range, input-device behavior,
keybinding syntax, and the complete action list.

### Nix (home-manager / NixOS)

Declarative config mirrors Noctalia: Nix attrsets are serialized to TOML with `pkgs.formats.toml`.

```nix
# flake input
umbriel.url = "path:/path/to/umbriel"; # or github:noctalia-dev/umbriel

# NixOS
imports = [ inputs.umbriel.nixosModules.default ];
programs.umbriel.enable = true;

# home-manager
imports = [ inputs.umbriel.homeModules.default ];
programs.umbriel = {
  enable = true;
  settings = {
    general = {
      terminal = "ghostty";
      autostart = [ "noctalia" ];
    };
    layout.gap = 5;
    input.keyboard.layout = "de";
    keybinds = {
      "Mod+Return" = "terminal-spawn";
      "Mod+Q" = "window-close";
      "Mod+R" = "spawn:noctalia msg panel-toggle launcher";
    };
  };
};
```

`settings` also accepts a raw TOML string or a path to a `.toml` file. A hjem module is exported as
`inputs.umbriel.hjemModules.default`.

The `appearance`, `layout`, `general`, `input`, and `keybinds` sections overlay compiled defaults, so a config file is
optional. Keyboard input supports XKB layout/variant and repeat settings; touchpads support tap-to-click and natural
scrolling, mice support natural scrolling, and cursor theme/size are configurable. Libinput options are applied only
when supported by the device.

Key names and modifiers are case-insensitive. `Mod` resolves to Alt in nested sessions and Super on DRM.
Bindings use `option-action` names (for example `window-close`, `config-reload`),
plus `workspace-switch:N`, `window-move-to-workspace:N`, and arbitrary shell commands with `spawn:command`.

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
  home-module.nix
  hjem-module.nix
  nixos-module.nix
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
- `wp_viewporter` (done)
- `wp_fractional_scale_v1` (done)
- `ext_data_control_v1` (done)
- `wp_primary_selection` / primary selection v1 (done)
- `zwlr_gamma_control_v1` (done)
- `zwlr_output_management_v1`
- `zwp_text_input_v3`
- `ext_foreign_toplevel_list_v1`
