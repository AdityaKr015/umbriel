project(
  'umbriel',
  ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.3',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'werror=false',
    'b_ndebug=if-release',
  ],
)

add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  language: 'cpp',
)
add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  language: 'c',
)

wayland_server_dep = dependency('wayland-server')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.32')
wlroots_dep = dependency('wlroots-0.20')
# Umbriel needs SceneFX APIs not in upstream 0.5 (ignore_alpha, shadow
# output clip). Prefer a system scenefx-0.5 that already has both (Nix
# applies the same diffs as scenefx.wrap in package.nix). Otherwise build
# the wrap subproject so diff_files apply.
# Probe the header text instead of compiling: SceneFX headers use C99
# `static` array sizes that break raw C++ compile checks.
fs = import('fs')
pkgconfig = find_program('pkg-config', required: false)
use_system_scenefx = false
if pkgconfig.found()
  scenefx_exists = run_command(pkgconfig, '--exists', 'scenefx-0.5', check: false)
  if scenefx_exists.returncode() == 0
    scenefx_cflags = run_command(pkgconfig, '--cflags-only-I', 'scenefx-0.5', check: true).stdout().split()
    foreach flag : scenefx_cflags
      if flag.startswith('-I')
        header = flag.substring(2) / 'scenefx' / 'types' / 'wlr_scene.h'
        if fs.exists(header)
          header_text = fs.read(header)
          if header_text.contains('wlr_scene_blur_set_ignore_alpha') and header_text.contains(
            'wlr_scene_shadow_set_output_clip'
          )
            use_system_scenefx = true
            break
          endif
        endif
      endif
    endforeach
    if not use_system_scenefx
      message('system scenefx-0.5 lacks Umbriel patches; using patched wrap subproject')
    endif
  endif
endif
if use_system_scenefx
  scenefx_dep = dependency('scenefx-0.5', allow_fallback: false)
else
  scenefx_proj = subproject(
    'scenefx',
    default_options: ['examples=false', 'tracy_enable=false', 'werror=false', 'default_library=static'],
  )
  scenefx_dep = scenefx_proj.get_variable('scenefx')
endif
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput')
pixman_dep = dependency('pixman-1')
tomlplusplus_dep = dependency('tomlplusplus')
cairo_dep = dependency('cairo')
pangocairo_dep = dependency('pangocairo')
libdrm_dep = dependency('libdrm')

# wlroots cursor-shape header pulls enum defs from wayland-protocols.
wayland_protocols_enums = declare_dependency(
  include_directories: include_directories(
    wayland_protocols_dep.get_variable(pkgconfig: 'includedir'),
    is_system: true,
  ),
)

wayland_scanner = find_program('wayland-scanner')

layer_shell_protocol_h = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

output_mgmt_client_h = custom_target(
  'wlr-output-management-unstable-v1-client-protocol.h',
  input: 'protocols/wlr-output-management-unstable-v1.xml',
  output: 'wlr-output-management-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
output_mgmt_client_c = custom_target(
  'wlr-output-management-unstable-v1-protocol.c',
  input: 'protocols/wlr-output-management-unstable-v1.xml',
  output: 'wlr-output-management-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

sources = files(
  'src/main.cpp',
  'src/core/animation.cpp',
  'src/core/log.cpp',
  'src/config/config.cpp',
  'src/config/config_merge.cpp',
  'src/config/config_watcher.cpp',
  'src/layout/insert_hint.cpp',
  'src/layout/scrolling.cpp',
  'src/layout/dwindle.cpp',
  'src/layout/layout.cpp',
  'src/server/server.cpp',
  'src/server/server_events.cpp',
  'src/server/server_focus.cpp',
  'src/output/output.cpp',
  'src/input/seat.cpp',
  'src/input/keyboard.cpp',
  'src/input/cursor.cpp',
  'src/input/gestures.cpp',
  'src/input/cursor_constraint.cpp',
  'src/view/view.cpp',
  'src/view/view_foreign.cpp',
  'src/view/popup.cpp',
  'src/layer/surface.cpp',
  'src/scene/surface_blur.cpp',
  'src/scene/surface_shadow.cpp',
  'src/scene/config_banner.cpp',
  'src/scene/text_buffer.cpp',
  'src/lock/session_lock.cpp',
  'src/workspace/workspace.cpp',
  'src/cli/outputs.cpp',
)

umbriel = executable(
  'umbriel',
  sources + [layer_shell_protocol_h, output_mgmt_client_h, output_mgmt_client_c],
  include_directories: include_directories('src'),
  dependencies: [
    scenefx_dep,
    wlroots_dep,
    wayland_server_dep,
    wayland_client_dep,
    wayland_protocols_enums,
    xkbcommon_dep,
    libinput_dep,
    pixman_dep,
    tomlplusplus_dep,
    cairo_dep,
    pangocairo_dep,
    libdrm_dep,
  ],
  install: true,
)

install_data(
  'data/umbriel.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
)

install_data(
  'data/umbriel-session.target',
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
)

