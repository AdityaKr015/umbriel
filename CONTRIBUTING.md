Contributing
===

This file collects contributor-facing details for Umbriel: design goals, stack notes, code style, source layout,
and debugging helpers. Umbriel shares its conventions with [noctalia](https://github.com/noctalia-dev/noctalia):
same team, same style. If in doubt, match what noctalia does.

For dependencies and normal build commands, start with [README.md](README.md).

## Design Principles

- Thin layer over wlroots 0.20 + SceneFX: lean on the libraries, do not reimplement them.
- Domain-oriented C++23: one domain per directory, headers beside their sources, and `src/` as the include root.
- Effects (blur, shadows, rounded corners, animations) go through SceneFX; patched APIs live in the fork rather than
  ad-hoc scene hacks.
- Mechanism and policy stay separate. Example: `View::applySeatFocus` is mechanism; focus policy lives in
  `Server::focusView`.
- Keep the compositor event loop single-threaded. `Server::spawn` relies on that property to make its `fork` and
  environment setup safe.
- Packaging targets Nix first, plus plain system packages via pkg-config.

## Stack

Direct project dependencies. Transitive dependencies are owned by their providing system packages.

| Layer | Library |
|-------|---------|
| Compositor framework | `wlroots-0.20` |
| Scene graph and effects | `SceneFX` (blur, shadows, rounded corners; patched fork, submodule) |
| Wayland core | `wayland-server`, `wayland-client`, `wayland-protocols`, `wayland-scanner` |
| Input | `libinput`, `xkbcommon` |
| Graphics | `pixman`, `libdrm`, OpenGL via wlroots |
| Text | `cairo`, `pangocairo` |
| Memory allocation | `jemalloc` (optional, glibc) |
| Config | `tomlplusplus` |
| JSON (IPC) | `nlohmann/json` |
| Xwayland | `xwayland-satellite` (managed at runtime) |

## Development Commands

The README covers routine builds and running Umbriel. Contributor checks and specialized builds use:

| Command | Purpose |
|---------|---------|
| `just configure <mode> [prefix]` | Create or reconfigure a build directory and symlink `compile_commands.json` to it |
| `just asan` | Build with AddressSanitizer |
| `just run <mode> [startup]` | Build and run a nested session, optionally spawning a command |
| `just test` | Run the Meson test suite |
| `just verify <mode> [filter]` | Run the interactive/visual regression harness (`tests/harness/verify.sh`) against a headless build |
| `just lint` | Rebuild without compiler warnings and run clang-tidy |
| `just format` | Format source and test files |
| `just install` | Build a release binary and install it with `meson install` |
| `just clean <mode>` | Remove a build directory |
| `just rebuild <mode>` | Clean and rebuild a build directory |

## Code Style

This project uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) for formatting, with the same
`.clang-format` as noctalia-shell (LLVM base, 2-space indent, 120 columns, left pointer alignment, regrouped includes).
Run `just format` before committing.

Static analysis uses [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) with the same `.clang-tidy` check set as
noctalia-shell. Run `just lint` (warnings are errors). Prefer the modern idioms the checks enforce: `auto`, ranges,
`std::print`/`std::format`, `make_unique`, scoped locks, no C-style casts, uppercase literal suffixes (`1.0F`).

`just configure <mode>` creates a root `compile_commands.json` symlink to the selected Meson build directory, so
clangd and clang-tidy see the build you are working in.

The repo also includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format`
and refreshes the git index for tracked formatting changes.

### Naming Conventions

| | Convention | Example |
|---|---|---|
| Files | snake_case | `session_lock.cpp` |
| Directories | snake_case | `input/`, `workspace/` |
| Types / Classes | PascalCase | `SessionLock` |
| Functions / Methods | camelCase | `focusView()` |
| Variables / Parameters | camelCase | `startupCmd` |
| Private members | m_camelCase | `m_sceneTree` |
| Constants | k-prefixed constexpr | `kLayerCount` |
| Macros | SCREAMING_SNAKE_CASE | `UMBRIEL_VERSION` |

Scoped enums (`enum class`) use PascalCase enumerators and an explicit `std::uint8_t` underlying type where it makes
sense: `enum class FocusReason : uint8_t { Directional, PointerPress, ... }`.

Getters are the noun, without a `get` prefix, and `[[nodiscard]]`: `toplevel()`, `mapped()`, `workspace()`.

### wlroots patterns

- Headers use `#pragma once`.
- Forward-declare `wlr_*` structs in headers; include the wlroots headers only in the `.cpp`. Wrap C includes in
  `extern "C" { ... }` when the header is not already C++-safe.
- Wire wlroots signals with the paired-handler pattern: a `static void onEvent(wl_listener*, void*)` trampoline that
  recovers `this` via `wl_container_of` and forwards to a `void handleEvent()` member. Store the `wl_listener` as an
  `m_event{}` member.
- Include ordering follows clang-format regrouping: project `"..."` headers first, then system `<...>` headers.

## Project Layout

```text
src/
  main.cpp
  wlr.h
  server/     display, backend, scene, protocol wiring, focus, and IPC
  output/     per-output lifecycle and frame commits
  input/      seat, keyboard, cursor, gestures, constraints, and IME relay
  view/       XDG toplevels and popups, window rules, and decoration
  layer/      layer-shell surfaces
  lock/       ext-session-lock surfaces
  xwayland/   xwayland-satellite process supervisor
  workspace/  per-output workspaces and scratchpads
  layout/     scrolling and dwindle layouts, insert and drop targets
  overview/   overview lifecycle and presentation
  scene/      blur, shadows, text, banners, and internal overlays
  config/     TOML parsing, resolution, reloads, and diagnostics
  core/       animation, logging, process, and resource helpers
  cli/        runtime inspection and command-line entry points
protocols/    vendored Wayland protocol XML
data/         session desktop entry
nix/          package and system integration modules
```

Conventions:

- `src/` is the include root; headers live next to their sources.
- Each directory owns one domain. Add new sources to the matching directory and register them in `meson.build`.
- Vendored Wayland protocol XML lives in `protocols/` and is code-generated via `wayland-scanner` in `meson.build`.
- User-facing configuration documentation lives in [`docs/user/`](docs/user/). Update it when adding or changing
  config options. The reference pages are linked from [`example.toml`](example.toml) and the
  [README](README.md#configuration). Maintainer design notes live in [`docs/design/`](docs/design/).

## SceneFX submodule

SceneFX is a git submodule tracking the `umbriel` branch of `noctalia-dev/scenefx`. Edit its sources in place, commit
in the submodule, and push to the fork.

## Debugging

Debug and ASan builds log at debug level to stderr and to `$XDG_CACHE_HOME/umbriel/umbriel.log`
(fallback `~/.cache/umbriel/umbriel.log`).

Run under AddressSanitizer with `just run asan`.

The CLI doubles as a runtime inspection and IPC surface against a running compositor:

```sh
umbriel validate [-c <config>]   # check a config file without starting
umbriel outputs                  # list connectors and modes
umbriel windows                  # list windows (focused *, urgent !)
umbriel layers                   # list layer-shell surfaces
umbriel keyboard-layouts         # list configured keyboard layouts
umbriel msg --help              # list actions available to `msg` and keybinds
umbriel msg <action> [args...]   # send an action to the running compositor
```

`windows`, `layers`, `keyboard-layouts`, and `msg` accept `--json` / `-j` for machine-readable output.

## Commits

Use [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): imperative summary`.
