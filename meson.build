project(
  'umbriel',
  ['cpp'],
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

wayland_server_dep = dependency('wayland-server')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.32')
wlroots_dep = dependency('wlroots-0.20')
scenefx_dep = dependency('scenefx-0.5')
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput')
pixman_dep = dependency('pixman-1')

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

sources = files(
  'src/main.cpp',
  'src/core/log.cpp',
  'src/server/server.cpp',
  'src/server/server_events.cpp',
  'src/server/server_focus.cpp',
  'src/output/output.cpp',
  'src/input/seat.cpp',
  'src/input/keyboard.cpp',
  'src/input/cursor.cpp',
  'src/input/cursor_constraint.cpp',
  'src/view/view.cpp',
  'src/view/view_foreign.cpp',
  'src/view/popup.cpp',
  'src/layer/surface.cpp',
  'src/lock/session_lock.cpp',
  'src/workspace/workspace.cpp',
)

umbriel = executable(
  'umbriel',
  sources + [layer_shell_protocol_h],
  include_directories: include_directories('src'),
  dependencies: [
    scenefx_dep,
    wlroots_dep,
    wayland_server_dep,
    wayland_protocols_enums,
    xkbcommon_dep,
    libinput_dep,
    pixman_dep,
  ],
  install: true,
)
