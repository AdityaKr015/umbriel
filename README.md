# Umbriel

Umbriel is a Wayland compositor designed for daily use, with scrolling and dwindle layouts, per-output workspaces,
window rules, blur, shadows, and fluid animations.

It runs independently and can be paired with [Noctalia](https://github.com/noctalia-dev/noctalia), which provides a
first-class desktop shell experience for Umbriel. Umbriel is built in C++23 on
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) and [SceneFX](https://github.com/wlrfx/scenefx), with
Xwayland support provided by [xwayland-satellite](https://github.com/Supreeeme/xwayland-satellite).

> [!IMPORTANT]
> Umbriel is young and actively evolving. It is usable today, but configuration keys, keybinds, and behavior may change
> between releases, and rough edges remain. Current defaults are opinions, not stability promises.

<p align="center">
  <img src="https://assets.noctalia.dev/?file=umbriel.svg" alt="Umbriel Logo" style="width: 192px" />
</p>

<p align="center">
  <a href="https://docs.noctalia.dev/umbriel/">
    <img src="https://img.shields.io/badge/docs-fbf099?style=for-the-badge&logo=gitbook&logoColor=110f3d&labelColor=fbf099" alt="Documentation" />
  </a>
  <a href="https://discord.noctalia.dev">
    <img src="https://img.shields.io/badge/discord-fbf099?style=for-the-badge&logo=discord&logoColor=110f3d&labelColor=fbf099" alt="Discord" />
  </a>
</p>

## Why Umbriel?

When people ask what Umbriel's selling point is, the honest answer is that there is no single killer feature. We were
simply disappointed with the choices available to us, so we built the compositor we wanted to live in. The plan is
not to conquer the world or take over the big names; it is to feel at home with something we have a say in, with less
friction. That is exactly how Noctalia came to life, and Umbriel is its compositor side.

To understand the values and philosophy guiding the project, read our [ethos](https://noctalia.dev/ethos).

## Features

- Scrolling and dwindle layouts with per-workspace selection, width presets, animated navigation, and mouse-driven
  resizing and tiled reordering
- Independent workspaces per output, with hotplug support and configurable modes, positions, scales, and transforms
- Floating, pinned, and fullscreen windows with configurable placement, focus, sizing, opacity, and visual effects
- Per-output scratchpads for temporarily hiding windows, with toggle, move, restore, and focus-next actions targetable at any output
- An animated overview, directional focus, configurable keybinds, submaps, and activation policy
- Blur, shadows, rounded corners, double borders, opacity, and animated position, size, and fade transitions
- Keyboard, pointer, touch, touchpad gestures, XKB configuration, and text-input-v3/input-method-v2 input method support
- Layer shell, session locking, clipboard management, screen capture, output control, and gamma control
- X11 application support through xwayland-satellite
- Live-reloaded TOML configuration with diagnostics and includes, plus local IPC and runtime inspection commands
- Runs as a nested Wayland compositor inside an existing Wayland or X11 desktop for development, or directly on DRM
  for daily use

## Building

After cloning, initialize the patched SceneFX fork tracked in `subprojects/scenefx`:

```sh
git submodule update --init
```

### System build

Install a C++23 compiler, Meson, Ninja, pkg-config, wayland-scanner, and development packages for wlroots 0.20,
Wayland, xkbcommon, libinput, pixman, libdrm, Cairo, Pango, tomlplusplus, and nlohmann-json. Then build Umbriel:

```sh
just debug
just release
```

`jemalloc` is optional but recommended on glibc: it returns freed memory to the OS promptly and bounds heap
fragmentation in long-running sessions. Meson's `-Djemalloc=enabled` or `-Djemalloc=disabled` forces the choice; the
default (`auto`) uses it when the development package is installed and skips it otherwise (non-glibc libc builds
always skip it).

The binaries are written to `build-debug/umbriel` and `build-release/umbriel`. Meson uses a system `scenefx-0.5`
only when its headers provide the required APIs; otherwise it builds the initialized submodule.

### Nix

Build the package directly:

```sh
nix build
```

The resulting binary is available at `result/bin/umbriel`. For development, enter the project shell and use the same
Just recipes as a system build:

```sh
nix develop
just debug
```

## Running

From an existing Wayland or X11 session, Umbriel opens a nested window (mod = Alt).
From a TTY it takes over the seat (mod = Super).

```sh
just run debug kitty
```

Or run the binary directly:

```sh
./build-debug/umbriel -s kitty
```

Inside the session:

| Shortcut | Action |
|----------|--------|
| mod+Escape | Quit (asks for confirmation) |
| mod+F1 | Cycle window focus |
| mod+H/J/K/L or arrows | Focus adjacent window |
| mod+Shift+H/J/K/L or arrows | Move focused window |
| mod+comma / mod+period | Consume left / expel right |
| mod+R / mod+F | Cycle width / toggle fullscreen |
| mod+T | Toggle floating for the focused window |
| mod+P | Toggle pin for the focused window |
| mod+O | Toggle the overview |
| mod+1..9 | Switch workspace on focused monitor |
| mod+Shift+1..9 | Move focused window to workspace and follow |

`kitty` is an optional startup command. Replace it with another command, or omit it by running `just run debug`
or `./build-debug/umbriel`. There is no default spawn keybind, so add one under `[keybinds]` (see
[`example.toml`](example.toml)) to open more terminals from inside the session, e.g. `"Mod+Return" = "spawn:kitty"`.

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
- [Maintainer design notes](docs/design/README.md): reloads, workspaces, and overview rendering

### Nix (home-manager / NixOS)

Declarative configuration uses Nix attrsets serialized to TOML with `pkgs.formats.toml`.

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
    general.autostart = [ "noctalia" ];
    layout.gap = 5;
    input.keyboard.layout = "de";
    keybinds = {
      "Mod+Return" = "spawn:kitty";
      "Mod+Q" = "window-close";
      "Mod" = "spawn:noctalia msg panel-toggle launcher";
    };
  };
};
```

`settings` also accepts a raw TOML string or a path to a `.toml` file. A hjem module is exported as
`inputs.umbriel.hjemModules.default`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, naming conventions, the dependency stack, and debugging
helpers. Umbriel shares its conventions with [noctalia](https://github.com/noctalia-dev/noctalia). For general help
and design discussion, join the community on [Discord](https://discord.noctalia.dev).

## License

MIT License. See [LICENSE](LICENSE) for details.
