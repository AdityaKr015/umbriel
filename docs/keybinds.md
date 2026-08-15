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

A modifier can also be bound by itself:

```toml
"Mod" = "spawn:noctalia msg panel-toggle launcher"
```

Modifier-only binds run on release when no other discrete input occurred while
the modifier was held. Any other key press, mouse button, scroll, touch down, or
gesture cancels the action. Pointer motion alone does not cancel it. Both the
left and right key for the logical modifier are accepted, and modifier-only
binds never repeat. Combinations containing only multiple modifiers, such as
`Ctrl+Alt`, are invalid.

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

Workspace selectors use exact names, including numeric names such as `1`.
Unique names resolve globally; duplicate names resolve on the preferred output.
Add `/output` to target another output explicitly. On a dynamic output, a
numeric target first uses the preferred output. If the number is beyond the
current workspace list, Umbriel uses the last workspace.

### Floating action

`window-toggle-floating` remembers the window's floating size and position.
The first time a window floats, Umbriel places it slightly below and to the
right of its tiled position while keeping it on-screen.

`window-toggle-pinned` makes the focused window float and keeps it above
fullscreen windows on its output. Pinned windows remain visible when you
switch workspaces. You cannot pin a fullscreen window, and making a pinned
window fullscreen removes its pinned state.

### Overview actions

Use `overview-toggle`, `overview-open`, or `overview-close`.

### Cheatsheet actions

Use `cheatsheet-toggle`, `cheatsheet-open`, or `cheatsheet-close`.

The cheatsheet lists every active keybind. It opens at startup when
`general.show_cheatsheet` is `true`, which is the default. You can also toggle
it through IPC with `umbriel msg cheatsheet-toggle`.

Any non-modifier key or mouse button closes the cheatsheet. Bound key
combinations still run normally. A click used to close the cheatsheet is not
passed to the window beneath it.

### Scratchpad actions

Each output has its own scratchpad for temporarily hiding windows.

| Action | What it does |
|--------|--------------|
| `window-move-to-scratchpad` | Move the focused window from its workspace into the scratchpad. |
| `scratchpad-toggle` | Show or hide the output's scratchpad windows. |
| `window-restore-from-scratchpad` | Return the focused scratchpad window to its saved workspace. |
| `scratchpad-focus-next` | Focus the next visible scratchpad window. |

When you show a scratchpad again, focus returns to the window that was focused
when you hid it. Add `:<output>` to any scratchpad action to target another
output, for example `scratchpad-toggle:DP-1`.

Scratchpad windows always float. Dragging one does not tile it or restore it to
the workspace beneath it.

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
"Mod" = "spawn:noctalia msg panel-toggle launcher"
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
