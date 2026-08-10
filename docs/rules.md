# Window and Layer Rules

## Window Rules

Window rules match windows by `app_id` and/or `title` using ECMAScript regex.
All matching `[[window_rule]]` entries are merged top-to-bottom; later rules
override earlier ones for each field they set (last-writer-wins).

```toml
[[window_rule]]
match.app_id = "firefox"
match.title = "^Library$"
default_floating = true
```

### Matching

| Selector | Type | Description |
|----------|------|-------------|
| `match.app_id` | regex | Match the window's app ID. |
| `match.title` | regex | Match the window's title. |
| `match.is_focused` | bool | Match the window's focused state dynamically. |

All selectors are optional. Omitting every selector matches all windows.
Matching uses regex search (partial match); use `^` and `$` to anchor.

Run `umbriel apps` to list running app IDs.

### One-shot effects (applied at map time)

These effects fire once when the window maps. If a window's title arrives after
mapping, rules are re-evaluated once.

| Key | Type | Description |
|-----|------|-------------|
| `default_output` | string | Open on a specific output (e.g. `"DP-1"`). |
| `default_floating` | bool | Force floating (`true`) or force tiling (`false`). |
| `default_size` | `[w, h]` | Initial size in pixels. Floats use both; tiled windows ignore height. |
| `default_width` | float | Column width fraction (0.1-1.0). Gap-aware: fractions that sum to 1 tile exactly. Overrides `layout.default_width_fraction`. |
| `default_workspace` | int | Place on workspace N (1-based). |
| `default_fullscreen` | bool | Map fullscreen. |
| `default_maximize` | bool | Map maximized. Tiled: column full-width. Floating: fill usable area. |

### Continuous effects (update on title/app_id/focus changes)

| Key | Type | Description |
|-----|------|-------------|
| `opacity` | float | Surface opacity (0.0-1.0). |
| `blur` | bool | Enable/disable blur for this window. |
| `blur_popups` | bool | Enable/disable blur for its XDG popups. |
| `blur_ignore_alpha` | float | Skip blur where surface alpha is below this threshold (0.0-1.0). Applies to the window and its popups. |
| `blur_optimized` | bool | Override `appearance.blur.optimized` for this window. |

### Examples

```toml
# Enable blur for every window
[[window_rule]]
blur = true

# Narrow columns for terminals and file managers
[[window_rule]]
match.app_id = "^(Alacritty|kitty|com\\.mitchellh\\.ghostty|org\\.gnome\\.Nautilus)$"
default_width = 0.33

# Wide columns for browsers
[[window_rule]]
match.app_id = "^(helium|chromium)$"
default_width = 0.75

# Slight transparency for editors and file managers
[[window_rule]]
match.app_id = "^(code|org\\.gnome\\.Nautilus)$"
opacity = 0.97

# Float utility windows
[[window_rule]]
match.app_id = "^(Emulator|zenity|xdg-desktop-portal|qalculate-gtk|org\\.pulseaudio\\.pavucontrol)$"
default_floating = true

# Float common dialogs by title
[[window_rule]]
match.title = "^(Open File|Select|Choose a wallpaper|Open Folder|Save As|Library|Choose Where to Download|File Operation Progress|Rename|Copy Files|Move Files|Search Files)"
default_floating = true

# Games on workspace 4, fullscreen
[[window_rule]]
match.app_id = "^(steam.*|overwatch|overwatch\\.exe)$"
default_workspace = 4

[[window_rule]]
match.app_id = "^(steam_proton|steam_app.*|overwatch|overwatch\\.exe)$"
default_fullscreen = true

# Noctalia settings
[[window_rule]]
match.app_id = "^dev.noctalia.Noctalia$"
default_floating = true
default_size = [1000, 900]
blur_popups = false

# Noctalia share picker
[[window_rule]]
match.app_id = "^dev.noctalia.UmbrielSharePicker$"
default_floating = true
default_size = [800, 600]

# Swash
[[window_rule]]
match.app_id = "^dev.lemmy.swash$"
default_floating = true
default_size = [1000, 900]

# Dim unfocused windows
[[window_rule]]
match.is_focused = false
opacity = 0.85

[[window_rule]]
match.is_focused = true
opacity = 1.0
```

---

## Layer Rules

Layer rules match layer-shell surfaces (bars, launchers, notifications) by
namespace using ECMAScript regex. Run `umbriel layers` to discover surface
namespaces.

```toml
[[layer_rule]]
match.namespace = "^noctalia-(bar-[^\"]+|notification|dock|panel|attached-panel|osd|desktop-widget-[^\"]*)$"
blur = true
blur_ignore_alpha = 0.5
blur_popups = true
```

### Matching

| Selector | Type | Description |
|----------|------|-------------|
| `match.namespace` | regex | Match the layer surface namespace. |

Matching uses regex search (partial match); use `^` and `$` to anchor.

### Effects

| Key | Type | Description |
|-----|------|-------------|
| `blur` | bool | Enable/disable blur for the layer surface. |
| `blur_popups` | bool | Enable/disable blur for descendant XDG popups. |
| `blur_ignore_alpha` | float | Skip blur where surface alpha is below this threshold (0.0-1.0). `0.0` blurs the entire rectangle; higher values leave transparent regions unblurred. |
| `blur_optimized` | bool | Override `appearance.blur.optimized`. |

Layer-shell blur is disabled by default. All matching rules are merged
top-to-bottom (last-writer-wins), same as window rules.
