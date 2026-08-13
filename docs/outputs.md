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
| `workspaces` | int or string array | `9` | Workspace inventory: a count (`5` creates `"1"` through `"5"`) or an ordered list of names (`["main", "code", "chat"]`). |
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

Each output may declare its own set of workspaces. Omitting `workspaces`
creates the default 9 numeric workspaces (`"1"` through `"9"`).

Reloading the config reconciles inventories live. Existing workspaces survive by
name, then by position; windows from removed workspaces move to the nearest
survivor.

---

# Workspace Rules

`[[workspace]]` entries customize the layout of an existing workspace. They
never create, remove, append, or rename workspaces (the inventory comes from
each output's `workspaces` setting above).

Each rule selects a workspace by exactly one of `name` (string) or `index`
(1-based integer). An optional `output` restricts the rule to that output.

## Inheritance

Layout fields inherit in this order:

```
[layout] -> matching global [[workspace]] -> matching output [[workspace]]
```

Rules without `output` apply wherever the selector matches. Output-specific
rules apply afterward and take precedence.

## Available fields

| Key | Type | Description |
|-----|------|-------------|
| `name` | string | Select by workspace name (mutually exclusive with `index`). |
| `index` | int | Select by 1-based position (mutually exclusive with `name`). |
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
