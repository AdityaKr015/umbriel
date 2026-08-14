# Umbriel

A Wayland compositor built on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.20 and
[SceneFX](https://github.com/wlrfx/scenefx).

Clean modular C++23 compositor with a scrolling layout, workspaces, window rules, shell protocols, and TOML configuration.

## Status

What works today:

- Nestable / DRM backend startup via wlroots + SceneFX renderer
- Output hotplug, modeset, and SceneFX-backed scene commits
- Seat, keyboard, pointer/cursor, xdg-shell toplevels and popups
- Per-monitor workspace inventories via `ext_workspace_manager_v1`, isolated and configurable per output
- Scrolling column layout with keyboard/mouse focus, movement, width presets, and animated transitions
- Floating windows with mouse move/resize, mod+drag tile reorder with drop-target preview
- Pinned floating windows above fullscreen applications, toggleable with `window-toggle-pinned`
- Window rules: per-app float, size, column width, workspace, fullscreen, activation focus, and opacity via `[[window_rule]]` with regex matching on app_id and title
- Blur, shadows, rounded corners, double borders (inner + outer ring), animated position/size/fade transitions, close animation snapshots
- Touchpad/trackpad gestures: 3-finger horizontal swipe (scroll layout), 3-finger vertical swipe (workspace switch), pinch/hold forwarding
- Touch input: tap-to-focus, touch forwarding to clients, hot-plug support
- Xwayland support via xwayland-satellite (managed lifecycle with respawn)
- `zwlr_layer_shell_v1` (anchors, exclusive zones, keyboard interactivity)
- `zwlr_foreign_toplevel_manager_v1` (active window / task list for shell clients)
- `zxdg_output_manager_v1` (logical size/position for shell clients)
- `ext_session_lock_manager_v1` (session lock / lock screen)
- `wp_cursor_shape_manager_v1` (named cursor shapes from clients)
- `zwp_pointer_constraints_v1` / `zwp_relative_pointer_manager_v1` (pointer lock/confine)
- `zwp_idle_inhibit_manager_v1` (+ `ext_idle_notifier_v1` for idle timers)
- `zwlr_screencopy_manager_v1` / `zwlr_export_dmabuf_manager_v1` (screenshots / capture)
- `xdg_activation_v1` (token-based window activation with configurable focus / urgency policy)
- `wp_viewporter` / `wp_fractional_scale_v1` (viewport crop + fractional scale)
- `ext_data_control_v1` (+ primary selection) for clipboard managers / history
- `zwlr_gamma_control_v1` (color temperature / night light)
- `zwlr_output_management_v1` (output configuration for shell clients)
- `xdg_decoration` / `server_decoration` (CSD/SSD preference via `appearance.prefer_no_csd`, default true)
- Configurable keybinds for focus/move, layout actions, applications, and workspaces (defaults when no config file)
- Scroll-wheel bindings (mod+WheelUp/Down for focus navigation)
- Output configuration: mode, position, scale, and transform per connector
- Sloppy focus (follows_mouse) with configurable scroll threshold
- Nested sessions use **Alt** as mod, native DRM uses **Super**
- Native DRM: Ctrl+Alt+F1..F12 switches VT
- Live config reload with file watcher, diagnostics banner, and include files
- `umbriel outputs` CLI subcommand for listing connectors and modes
- Nonblocking local IPC for commands and state queries, with per-connection deadlines
- Clean shutdown on `SIGINT` / `SIGTERM` / mod+Escape
- Noctalia shell runs against the protocols above

Still open / planned:

| Area | Direction |
|------|-----------|
| Layouts | Vertical scrolling, dwindle, master |
| Shell | IME, remaining Noctalia polish |
| Overview | Undecided |
| Input | Tablet |
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

Nix builds use the patched SceneFX submodule in `subprojects/scenefx`, so the Nix package and local
Meson builds consume the same source.

## Build (system packages)

If `wlroots-0.20`, `scenefx-0.5`, and `tomlplusplus` are available via pkg-config:

```sh
just debug
./build-debug/umbriel
```

Umbriel uses a patched SceneFX fork (`noctalia-dev/scenefx`, branch `umbriel`)
tracked as a git submodule in `subprojects/scenefx`. After cloning, run
`git submodule update --init`. Meson uses system `scenefx-0.5` only when the
required APIs are present in the headers; otherwise it builds from the submodule.
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
| mod+P | Toggle pin for the focused window |
| mod+1..9 | Switch workspace on focused monitor |
| mod+Shift+1..9 | Move focused window to workspace and follow |

Set `TERMINAL` to your terminal binary (`ghostty`, `kitty`, `alacritty`, ...).

Stop with mod+Escape or `Ctrl+C` from the parent terminal.

## Configuration

Umbriel loads `$XDG_CONFIG_HOME/umbriel/config.toml` (normally `~/.config/umbriel/config.toml`) at startup.
Pass `-c path/to/config.toml` to use another file. Config files can include other TOML files with
`[include] files = ["theme.toml", "keybinds.toml"]`; later files and the main file override earlier values.

See [`example.toml`](example.toml) for defaults and [`docs/`](docs/) for the full reference:

- [Configuration](docs/configuration.md): general, appearance, layout, input
- [Keybinds](docs/keybinds.md): chords, submaps, Noctalia integration
- [Window and Layer Rules](docs/rules.md): matching, effects, blur
- [Outputs](docs/outputs.md): monitors, workspaces, workspace layout overrides

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
      "Mod+Return" = "spawn:kitty";
      "Mod+Q" = "window-close";
      "Mod+R" = "spawn:noctalia msg panel-toggle launcher";
    };
  };
};
```

`settings` also accepts a raw TOML string or a path to a `.toml` file. A hjem module is exported as
`inputs.umbriel.hjemModules.default`.

Most settings overlay compiled defaults, so a config file is optional. Keybinds use compiled defaults only when no
config file is present; once a config file exists, `[keybinds]` is the complete set. The `environment` table exports arbitrary
environment variables (string values) to Umbriel and its spawned children. Window rules match on `app_id` and
`title` (regex) and optionally `is_focused` (boolean). Toplevel, layer, and XDG popup blur is opt-in through
`blur = true` and `blur_popups = true`; `[appearance.blur].enabled` is the master switch. Rules can configure alpha
masking with `blur_ignore_alpha` and optimized blur with `blur_optimized`. Window rules can also set floating, size,
workspace, fullscreen, and opacity. Keyboard input supports XKB layout/variant and repeat settings; touchpads support
tap-to-click and natural scrolling, mice support natural scrolling, and cursor theme/size are configurable. Libinput
options are applied only when supported by the device.

## Project layout

```text
src/
  main.cpp
  wlr.h
  server/     display, backend, scene, xdg/layer/lock wiring
  output/     per-output lifecycle and frame commits
  input/      seat, keyboard, cursor, gestures
  view/       xdg toplevels and popups, window rules
  layer/      layer-shell surfaces
  lock/       ext-session-lock surfaces
  workspace/  per-monitor workspaces (ext-workspace)
  layout/     scrolling column layout, insert hint
  scene/      blur, shadows, text buffer, config banner
  config/     TOML parsing, live reload, file watcher, merge
  core/       animation, logging
  cli/        output listing subcommand
protocols/    vendored Wayland protocol XML
data/         wayland-sessions desktop entry
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
- `zwlr_output_management_v1` (done)
- `ext_foreign_toplevel_list_v1` (done)
- `zwp_text_input_v3`

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, naming conventions, the dependency stack, and debugging
helpers. Umbriel shares its conventions with [noctalia-shell](https://github.com/noctalia-dev/noctalia).
