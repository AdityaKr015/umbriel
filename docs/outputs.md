# Outputs

Output sections configure individual monitors. Names must exactly match
connector names such as `DP-1` or `HDMI-A-1`. Nested outputs use `WL-1`;
headless outputs use `HEADLESS-1`.

Run `umbriel outputs` inside a session to list connector names and modes.

```toml
[output.DP-1]
mode = "3840x2160@165"
position = [0, 0]
scale = 1.25
workspaces = 5
```

## Settings

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `mode` | string | (native) | Resolution and refresh rate: `"WIDTHxHEIGHT"` or `"WIDTHxHEIGHT@HZ"`. Fractional Hz allowed. Ignored in nested sessions (the parent controls size). |
| `position` | `[x, y]` | (auto) | Layout coordinates. |
| `scale` | float | (auto) | Output scale (0.25-4.0). |
| `workspaces` | int, string array, or `"dynamic"` | `"dynamic"` | Dynamic numbered workspaces, a static count from 1 to 64, or a static ordered list of 1 to 64 names. |
| `transform` | string | `"normal"` | Output rotation/flip. |

### Transform values

`normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`,
`flipped-270`.

## Multi-monitor example

A triple-monitor setup with a 4K primary, a 1440p top monitor, and a 1080p
side panel:

```toml
[output.DP-1]
mode = "3840x2160@165"
position = [0, 0]
scale = 1.25
workspaces = 5

[output.DP-2]
mode = "2560x1440@144"
position = [1300, -1440]
scale = 1.0
workspaces = ["VIDEO"]

[output.HDMI-A-1]
mode = "1920x1080@60"
position = [3072, 0]
scale = 1.0
workspaces = ["CHAT", "STATS"]
```

Tiled windows are clipped to the logical bounds of their owning output.
Partially visible scrolling columns do not render onto adjacent outputs,
including when either output uses fractional scaling.

## Machine-specific overrides

A common pattern is to keep output configuration in a separate per-machine
include file so the same base config works on different hardware:

```toml
# ~/.config/umbriel/config.toml
[include]
files = [
  "src/general.toml",
  "src/keybinds.toml",
  "machines/monolith.toml",   # output config for this machine
]
```

## Workspace inventory

Omitting `workspaces` or setting it to `"dynamic"` enables auto-managed
numbered workspaces. A dynamic output starts with one empty workspace named
`"1"`. When the trailing workspace gains a window, Umbriel appends another
empty workspace. Empty workspaces are removed after they are left, except when
still active, and the remaining workspaces are renumbered.

An explicit count or name list creates a fully static inventory containing
exactly those workspaces. Static workspaces are not removed when empty.
Numeric switch targets beyond the current dynamic workspace count clamp to the
last workspace.

Reloading the config reconciles inventories live. Existing static workspaces
survive by name, then by position; windows from removed static workspaces move
to the nearest survivor. Switching to dynamic mode preserves populated and
active workspaces, renumbers them, and appends a trailing empty workspace.

Layout-only reloads refresh workspace geometry without reconciling the
inventory or reapplying output mode, scale, transform, or placement. Border
width changes also refresh workspace spacing because borders contribute to the
resolved tile gap.

---

# Workspace Rules

`[[workspace]]` entries customize the layout of static inventory entries or
numbered dynamic workspace positions. They do not create workspaces directly.

Each rule selects a workspace by exactly one of `name` (string) or `index`
(1-based integer from 1 to 64). An optional `output` restricts the rule to that
output.

## Inheritance

Layout fields inherit in this order:

```
[layout] -> matching global [[workspace]] -> matching output [[workspace]]
```

Rules without `output` apply wherever the selector matches. Output-specific
rules apply afterward and take precedence.
On dynamic outputs, rules match numbered positions and names as those
workspaces exist.

## Available fields

| Key | Type | Description |
|-----|------|-------------|
| `name` | string | Select by workspace name (mutually exclusive with `index`). |
| `index` | int | Select by 1-based position from 1 to 64 (mutually exclusive with `name`). |
| `output` | string | Restrict to this output. |
| `layout.mode` | string | `"scrolling"` or `"dwindle"`. |
| `layout.gap` | int | Gap in pixels (0-500). |
| `layout.width_presets` | float array | Widths used by the width-cycle action in both layouts. |
| `layout.scrolling.default_width_fraction` | float | Initial scrolling column width (0.1-1.0). |
| `layout.scrolling.always_center_single_column` | bool | Center a lone scrolling column when narrower than the viewport. When disabled, the left-aligned column has a fixed left edge and resizes from its right edge. |

## Examples

```toml
# Dwindle layout for the VIDEO workspace on DP-2
[[workspace]]
output = "DP-2"
name = "VIDEO"
layout.mode = "dwindle"

# Scrolling for CHAT, dwindle for STATS, both on HDMI-A-1
[[workspace]]
output = "HDMI-A-1"
name = "CHAT"
layout.mode = "scrolling"

[[workspace]]
output = "HDMI-A-1"
name = "STATS"
layout.mode = "dwindle"

# Customize workspace position 4 on DP-1
[[workspace]]
index = 4
output = "DP-1"
layout.gap = 0
layout.scrolling.default_width_fraction = 0.667
```
