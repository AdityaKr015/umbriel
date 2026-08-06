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
  cairo,
  pango,
  libGL,
  libdrm,
  scenefx,
  tomlplusplus,
  xwayland-satellite,
  makeBinaryWrapper,
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
    makeBinaryWrapper
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    wayland
    wayland-protocols
    # SceneFX before wlroots so its scene symbols win at link time.
    scenefx
    wlroots_0_20
    libxkbcommon
    libinput
    pixman
    tomlplusplus
    libGL
    libdrm
    cairo
    pango
  ];

  mesonBuildType = "release";

  # Session desktop comes from meson (`data/umbriel.desktop`).
  # Rewrite Exec= so display managers invoke the wrapped binary in this store path.
  postInstall = ''
    if [ -f "$out/share/wayland-sessions/umbriel.desktop" ]; then
      substituteInPlace "$out/share/wayland-sessions/umbriel.desktop" \
        --replace-fail 'Exec=umbriel' "Exec=$out/bin/umbriel"
    fi
    wrapProgram $out/bin/umbriel \
      --prefix PATH : ${lib.makeBinPath [ xwayland-satellite ]} \
  '';

  passthru.providedSessions = [ "umbriel" ];

  meta = with lib; {
    description = "A Wayland compositor built on wlroots and SceneFX";
    homepage = "https://github.com/noctalia-dev/umbriel";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "umbriel";
  };
}
