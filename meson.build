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
wlroots_dep = dependency('wlroots-0.20')
scenefx_dep = dependency('scenefx-0.5')
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput')
pixman_dep = dependency('pixman-1')

sources = files(
  'src/main.cpp',
  'src/server/server.cpp',
  'src/output/output.cpp',
  'src/input/seat.cpp',
  'src/input/keyboard.cpp',
  'src/input/cursor.cpp',
  'src/view/view.cpp',
  'src/view/popup.cpp',
)

umbriel = executable(
  'umbriel',
  sources,
  include_directories: include_directories('src'),
  dependencies: [
    scenefx_dep,
    wlroots_dep,
    wayland_server_dep,
    xkbcommon_dep,
    libinput_dep,
    pixman_dep,
  ],
  install: true,
)
