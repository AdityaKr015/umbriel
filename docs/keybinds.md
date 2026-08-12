# Keybinds

All keybinds live under `[keybinds]`. Chords are case-insensitive.

```toml
[keybinds]
"Mod+T" = "spawn:kitty"
"Mod+Shift+Q" = "window-close"
"Mod+I" = "overview-toggle"
```

## Modifiers

| Modifier | Notes |
|----------|-------|
| `Mod` | Alt when nested, Super on bare-metal (DRM). |
| `Shift` | |
| `Ctrl` / `Control` | |
| `Alt` | |
| `Super` / `Logo` / `Win` | |

Bare keys are also allowed (e.g. `XF86AudioMute`).

## Special keys

**Scroll wheel:** `WheelUp`, `WheelDown`, `WheelLeft`, `WheelRight` (require
at least one modifier).

**Mouse buttons:** `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`,
`MouseForward` (require at least one modifier).

**Defaults:** `Mod+WheelUp` = `window-focus-left`, `Mod+WheelDown` =
`window-focus-right`.

## Actions

Run `umbriel actions` for the full list. Default keybinds are only loaded when
no config file exists; once you provide a config, `[keybinds]` is the complete
set.

### Parameterized actions

| Action | Parameter | Example |
|--------|-----------|---------|
| `spawn:<cmd>` | Shell command | `"spawn:kitty"` |
| `workspace-switch:<ws>` | Workspace name, optionally `/<output>` | `"workspace-switch:3"`, `"workspace-switch:CHAT/HDMI-A-1"` |
| `window-move-to-workspace:<ws>` | Same as above | `"window-move-to-workspace:2"` |
| `window-set-width:<frac>` | Fraction 0.1-1.0 | `"window-set-width:0.667"` |

Workspace selectors are exact names (including numeric ones like `1`). Unique
names resolve globally; duplicate names resolve on the preferred output. Append
`/output` to target a different output explicitly.

### Overview actions

`overview-toggle`, `overview-open`, `overview-close`.

### Cheatsheet actions

`cheatsheet-toggle`, `cheatsheet-open`, `cheatsheet-close`.

### Scratchpad actions

`window-move-to-scratchpad` minimizes the focused window into the current
output's scratchpad. `scratchpad-toggle` shows or hides that output's pool.
`window-restore-from-scratchpad` returns its focused window to its saved
workspace, and `scratchpad-focus-next` cycles visible scratchpad windows.
Append `:<output>` to any of these actions to target its per-output pool from
anywhere, for example `scratchpad-toggle:DP-1` or
`window-move-to-scratchpad:DP-1`.
Scratchpad windows always remain floating; dragging them never tiles or
restores them into the workspace underneath.

The keybinds cheatsheet overlay lists every active keybind in a styled panel.
It appears automatically on startup when `general.show_cheatsheet` is `true`
(the default). At runtime, toggle it via IPC (`umbriel msg cheatsheet-toggle`)
or bind one of the actions above. Any non-modifier key press dismisses the
overlay; bound chords still execute normally.

## Repeat

Binds repeat while held, using `input.keyboard.repeat_rate` and
`repeat_delay`. Opt out per bind with the table form:

```toml
"Mod+Return" = { action = "spawn:kitty", repeat = false }
```

Scratchpad visibility and cycling actions never repeat, even if their binding
does not set `repeat = false`.

## Submaps

Submaps are temporary keybind layers that can be nested. Enter with
`submap:<name>`, exit one level with `submap:reset`.

Binds inside a submap prefix the chord with `submap[name],`:

```toml
"Mod+S" = "submap:screencapture"
"submap[screencapture],1" = "spawn:grim screenshot.png"
"submap[screencapture],2" = "submap:region"
"submap[screencapture],Escape" = "submap:reset"
"submap[region],R" = "spawn:grim -g 'slurp -p' screenshot.png"
"submap[region],Escape" = "submap:reset"
```

A `submap:reset` bound in the default context (no prefix) always matches, even
inside a submap, as a global emergency exit:

```toml
"Escape" = "submap:reset"
```

## Example: Noctalia shell integration

[Noctalia](https://github.com/noctalia-dev/noctalia) exposes panels, screenshots,
and widgets via `noctalia msg`. Typical bindings:

```toml
"Mod+R" = "spawn:noctalia msg panel-toggle launcher"
"Mod+Z" = "spawn:noctalia msg panel-toggle launcher /emo"
"Mod+V" = "spawn:noctalia msg panel-toggle clipboard"
"Mod+W" = "spawn:noctalia msg panel-toggle wallpaper"
"Mod+N" = "spawn:noctalia msg panel-toggle noctalia/notes:panel"
"Mod+X" = "spawn:noctalia msg bar-toggle"
"Mod+P" = "spawn:noctalia msg screenshot-region"
"Mod+Shift+P" = "spawn:noctalia msg screenshot-fullscreen"
"Mod+Shift+W" = "spawn:noctalia msg desktop-widgets-toggle-edit"
"Mod+Escape" = "spawn:noctalia msg panel-toggle session"
```

## Example: direct column widths

```toml
"Mod+A" = "window-set-width:0.333"
"Mod+S" = "window-set-width:0.5"
"Mod+D" = "window-set-width:0.667"
"Mod+F" = "window-set-width:1.0"
```

## Example: scroll-wheel navigation

```toml
"Mod+WheelUp" = "window-focus-left"
"Mod+WheelDown" = "window-focus-right"
"Mod+Shift+WheelUp" = "column-move-left"
"Mod+Shift+WheelDown" = "column-move-right"
"Mod+MouseMiddle" = "overview-toggle"
```

## Example: media and brightness keys

```toml
# Volume (via Noctalia OSD)
"XF86AudioRaiseVolume" = "spawn:noctalia msg volume-up 2%"
"XF86AudioLowerVolume" = "spawn:noctalia msg volume-down 2%"

# Media playback (playerctl)
"XF86AudioPlay" = "spawn:playerctl play-pause"
"XF86AudioNext" = "spawn:playerctl next"
"XF86AudioPrev" = "spawn:playerctl previous"

# Brightness
"XF86MonBrightnessUp" = "spawn:brightnessctl set +5%"
"XF86MonBrightnessDown" = "spawn:brightnessctl set 5%-"
```
