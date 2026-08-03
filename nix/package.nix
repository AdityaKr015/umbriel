{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  wlroots_0_20,
  libxkbcommon,
  libinput,
  pixman,
  libGL,
  libdrm,
  scenefx,
  tomlplusplus,
}:
let
  inherit (builtins) head match readFile;
  version = head (match ".*\n  version: '([0-9][^']+)'.*" (readFile ../meson.build));
in
stdenv.mkDerivation {
  pname = "umbriel";
  inherit version;

  src = lib.cleanSource ./..;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    wayland
    wayland-protocols
    wlroots_0_20
    scenefx
    libxkbcommon
    libinput
    pixman
    tomlplusplus
    libGL
    libdrm
  ];

  mesonBuildType = "release";

  meta = with lib; {
    description = "A Wayland compositor built on wlroots and SceneFX";
    homepage = "https://github.com/noctalia-dev/umbriel";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "umbriel";
  };
}
