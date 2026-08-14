# Configuration

Umbriel reads TOML configuration from `~/.config/umbriel/config.toml`, or from a
path passed with `umbriel -c <path>`. Changes apply immediately on save; invalid
values keep the previous state. You can also bind `config-reload` to a key for
manual reloads.

Reloads are transactional. A parse failure keeps the committed configuration
and live compositor state, while diagnostics and include watches update so
fixing an included file can recover automatically. Successful reloads with no
runtime effects leave active overlays and output state untouched.

## Include

```toml
[include]
files = ["appearance.toml", "keybinds.toml"]
```

Paths are resolved relative to the main config file. Later files override
earlier files, and values in the main file override every include.

You can split your config into multiple files for clarity:

```toml
# ~/.config/umbriel/config.toml
[include]
files = [
  "src/general.toml",
  "src/appearance.toml",
  "src/input.toml",
  "src/keybinds.toml",
  "src/rules.toml",
  "src/workspaces.toml",
  "machines/monolith.toml",
]
```

## General

```toml
[general]
autostart = ["noctalia", "kitty"]
xwayland = true
show_cheatsheet = true
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `autostart` | string array | `[]` | Shell commands run once after startup. Never re-run on config reload. |
| `xwayland` | bool | `true` | Spawn `xwayland-satellite` for X11 app support. The binary must be installed. Changing this requires a restart. |
| `show_cheatsheet` | bool | `true` | Show the keybinds cheatsheet overlay on startup. Press any key or any mouse button to dismiss, or toggle at runtime via `cheatsheet-toggle`. |

## Environment

```toml
[environment]
GTK_THEME = "Adwaita:dark"
QT_QPA_PLATFORMTHEME = "qt5ct"
```

Extra environment variables exported to Umbriel and all spawned commands.
All values must be strings.

## Workspaces

```toml
[workspaces]
back_and_forth = true
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `back_and_forth` | bool | `false` | Re-selecting the active workspace jumps back to the previously active workspace on that output. |

Output workspaces are dynamic by default. See [Outputs](outputs.md) for dynamic
behavior and static per-output inventories, and
[Outputs: Workspace Rules](outputs.md#workspace-rules) for per-workspace layout
overrides.

## Appearance

```toml
[appearance]
prefer_no_csd = true
border_width = 2               # 0-100
outer_border_width = 0         # 0-100
corner_radius = 10             # 0-500, 0 disables
border_focused = "#7AA3FFFF"   # #RRGGBB or #RRGGBBAA
border_unfocused = "#292933FF"
scratchpad_border_focused = "#E5C07BFF"
scratchpad_border_unfocused = "#5C4A2AFF"
outer_border_color = "#1A1A1FFF"
insert_hint_color = "#7FC8FF80"
backdrop_color = "#000000FF"
animation_ms = 250             # 1-10000
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `prefer_no_csd` | bool | `true` | Ask clients to omit client-side decorations (xdg-decoration). Clients that explicitly request CSD are still honored. Restart apps after changing. |
| `border_width` | int | `2` | Inner border width in logical pixels (0-100), including around rounded corners. |
| `outer_border_width` | int | `0` | Ring outside the inner border in logical pixels (0-100). |
| `corner_radius` | int | `10` | Rounded corner radius (0-500). 0 disables. |
| `border_focused` | color | `#7AA3FFFF` | Border color for the focused window. |
| `border_unfocused` | color | `#292933FF` | Border color for unfocused windows. |
| `scratchpad_border_focused` | color | `#E5C07BFF` | Border color for the focused scratchpad window. |
| `scratchpad_border_unfocused` | color | `#5C4A2AFF` | Border color for unfocused scratchpad windows. |
| `outer_border_color` | color | `#1A1A1FFF` | Outer border color (no focus variant). |
| `insert_hint_color` | color | `#7FC8FF80` | Drop-target preview during drag. |
| `backdrop_color` | color | `#000000FF` | Background for fullscreen gaps and lock screen. |
| `animation_ms` | int | `250` | Animation duration in milliseconds (1-10000). |

Colors are `#RRGGBB` or `#RRGGBBAA`.

### Blur

```toml
[appearance.blur]
enabled = true
optimized = true
passes = 3        # 0-8
radius = 5        # 0-100
noise = 0.02      # 0.0-1.0
brightness = 0.9  # 0.0-2.0
contrast = 0.9    # 0.0-2.0
saturation = 1.1  # 0.0-2.0
```

`enabled` is the master switch. Individual surfaces must still opt in through
[window rules](rules.md) or [layer rules](rules.md#layer-rules).
Blur only renders where a surface is transparent.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | bool | `true` | Master blur switch. |
| `optimized` | bool | `true` | Cache one background blur per output instead of recomputing per surface. |
| `passes` | int | `3` | Blur passes (0-8). 0 disables. |
| `radius` | int | `5` | Blur radius (0-100). 0 disables. |
| `noise` | float | `0.02` | Noise overlay (0.0-1.0). |
| `brightness` | float | `0.9` | Brightness adjustment (0.0-2.0). |
| `contrast` | float | `0.9` | Contrast adjustment (0.0-2.0). |
| `saturation` | float | `1.1` | Saturation adjustment (0.0-2.0). |

### Shadow

```toml
[appearance.shadow]
enabled = true
softness = 10      # 0-200
offset_x = 2       # -200 to 200
offset_y = 2
color = "#0000008C"
```

Drop shadow behind windows (tiled and floating). Hidden while fullscreen.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | bool | `true` | Enable drop shadows. |
| `softness` | int | `10` | Gaussian blur sigma in pixels (0-200). 0 produces a hard-edged shadow. |
| `offset_x` | int | `2` | Horizontal shadow offset (-200 to 200). |
| `offset_y` | int | `2` | Vertical shadow offset (-200 to 200). |
| `color` | color | `#0000008C` | Shadow color. |

## Overview

```toml
[overview]
zoom = 0.5                     # 0.1-0.75
background_tint = "#10101430"
workspace_background = "#00000044"
```

Zoomed-out view of every workspace on every output (default `Mod+O`). Windows
stay live: click to focus, middle-click to close, drag between workspaces, or
4-finger swipe to toggle. Move up and down the filmstrip with the wheel, the
arrow keys, or a 3-finger swipe; because the real windows are hidden while the
overview is up, each of those steps one workspace at a time rather than sliding
the way a 3-finger swipe does outside. Dragged cards render at half opacity so
the insertion preview remains visible beneath them. Client-provided alpha is
multiplied by the drag opacity. Dwindle drags preview the directional split
under the pointer before the window is dropped.
Transparent windows keep their configured window-rule blur throughout the zoom
transition. Each workspace has a rounded background behind its cards; use the
configured alpha for anything from a subtle tint to an opaque fill.
Cards carry the same decoration as the real windows: the inner border in its
focus colour, the outer border ring, and the corner radius, all scaled to the
thumbnail. A card that a workspace row pushes past the edge of its output is cut
there, and the cut corners lose their radius, as a window spanning outputs does.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `zoom` | float | `0.5` | Workspace scale when fully zoomed out (0.1-0.75). |
| `background_tint` | color | `#10101430` | Tint composited over the desktop background. Alpha `00` leaves it untouched; `FF` hides it. |
| `workspace_background` | color | `#00000044` | Rounded background behind each workspace. Alpha `00` makes it invisible; `FF` makes it opaque. |

## Layout

```toml
[layout]
mode = "scrolling"                  # "scrolling" or "dwindle"
gap = 8                             # 0-500
width_presets = [0.333, 0.5, 0.667]

[layout.scrolling]
default_width_fraction = 0.5        # 0.1-1.0
center_underfull_strip = true
```

Shared layout options:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `mode` | string | `"scrolling"` | Layout algorithm: `"scrolling"` or `"dwindle"`. |
| `gap` | int | `8` | Gap between windows in pixels (0-500). |
| `width_presets` | float array | `[0.333, 0.5, 0.667]` | Widths visited by the `window-cycle-width` action in both layouts. |

Scrolling layout options:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `default_width_fraction` | float | `0.5` | Initial width assigned to new scrolling columns (0.1-1.0). |
| `center_underfull_strip` | bool | `true` | Center the complete strip whenever it is narrower than the viewport. Set to `false` to align an underfull strip at the left edge. |

Horizontal resizing updates underfull-strip centering continuously.

Dragged windows render at half opacity while the grab is active, keeping the
insertion preview visible through the window. The drag multiplier composes with
client-provided transparency and window-rule opacity.

When dragging a column, insertion previews occupy the free space beside the
actual column edges, including centered strips. For overflowing strips, the
extreme right edge of the output targets the end of the strip even when later
columns are off-screen.

Layout fields can be overridden per-workspace; see
[Workspace Rules](outputs.md#workspace-rules).

## Input

### Keyboard

```toml
[input.keyboard]
layout = ""       # XKB layout, empty = system default
variant = ""      # XKB variant
repeat_rate = 25  # 0-1000 Hz, 0 disables
repeat_delay = 600 # 0-10000 ms
```

### Touchpad

```toml
[input.touchpad]
tap = true
natural_scroll = true
```

Options are applied only when supported by the libinput device. Omit to
preserve each device's defaults.

### Mouse

```toml
[input.mouse]
natural_scroll = false
scroll_wheel_step = 60  # 1-1000, pixels per step for layout-scroll-left/right
```

Omit `natural_scroll` to preserve each device's default. `layout-scroll-left`
and `layout-scroll-right` clamp to the strip bounds, so the columns never park
past either edge.

### Cursor

```toml
[input.cursor]
theme = ""   # empty = environment/default Xcursor theme
size = 24    # 1-512
```

Cursor theme and size changes apply on config reload. Output scale changes also
reload the cursor image at the matching scale without requiring a restart.

### Focus

```toml
[input.focus]
follows_mouse = false
follows_mouse_max_scroll = 0.5  # optional, measured in viewport widths
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `follows_mouse` | bool | `false` | Focus the window under the pointer (sloppy focus). Only fires when the pointer enters a different window, then scrolls it on-screen. |
| `follows_mouse_max_scroll` | float | (no limit) | Refuse focus when bringing the window into view would scroll further than this. Measured in viewport widths, so `1.0` is one full screen and values above that are meaningful — revealing a column three screens away is `3.0`. `0.0` allows focus only for windows already fully visible. Omit for no limit. |

Values are clamped to `0.0`–`100.0`, and a clamp is reported. A negative limit
would refuse focus even for a window needing no scroll at all, which switches
hover focus off rather than limiting it.
