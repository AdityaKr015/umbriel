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
  '-DUMBRIEL_VERSION="' + meson.project_version() + '"',
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
# SceneFX: prefer a system installation, fall back to the git submodule.
# Detect if the patched APIs (ignore_alpha) are available and define
# HAVE_PATCHED_SCENEFX so the code can conditionally use them.
fs = import('fs')
pkgconfig = find_program('pkg-config', required: false)
use_system_scenefx = false
has_patched_scenefx = false
if pkgconfig.found()
  scenefx_exists = run_command(pkgconfig, '--exists', 'scenefx-0.5', check: false)
  if scenefx_exists.returncode() == 0
    use_system_scenefx = true
    scenefx_cflags = run_command(pkgconfig, '--cflags-only-I', 'scenefx-0.5', check: true).stdout().split()
    foreach flag : scenefx_cflags
      if flag.startswith('-I')
        header = flag.substring(2) / 'scenefx' / 'types' / 'wlr_scene.h'
        if fs.exists(header)
          header_text = fs.read(header)
          if header_text.contains('wlr_scene_blur_set_ignore_alpha')
            has_patched_scenefx = true
          endif
          break
        endif
      endif
    endforeach
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
  # Check if the submodule has patched APIs
  scenefx_subproject_dir = meson.current_source_dir() / 'subprojects' / 'scenefx'
  scenefx_subproject_header = scenefx_subproject_dir / 'include' / 'scenefx' / 'types' / 'wlr_scene.h'
  if fs.exists(scenefx_subproject_header)
    header_text = fs.read(scenefx_subproject_header)
    if header_text.contains('wlr_scene_blur_set_ignore_alpha')
      has_patched_scenefx = true
    endif
  endif
endif
if has_patched_scenefx
  add_project_arguments('-DHAVE_PATCHED_SCENEFX', language: 'cpp')
  add_project_arguments('-DHAVE_PATCHED_SCENEFX', language: 'c')
endif
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput')
pixman_dep = dependency('pixman-1')
tomlplusplus_dep = dependency('tomlplusplus')
nlohmann_json_dep = dependency('nlohmann_json')
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
  'src/cli/ipc_client.cpp',
  'src/server/ipc.cpp',
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
    nlohmann_json_dep,
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

