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
  xwayland-satellite,
  makeBinaryWrapper,
}:
let
  inherit (builtins) head match readFile;
  version = head (match ".*\n  version: '([0-9][^']+)'.*" (readFile ../meson.build));

  # Umbriel needs ignore_alpha on scene blur (not upstream in scenefx 0.5 yet).
  scenefxPatched = scenefx.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [
      ../subprojects/packagefiles/scenefx-blur-ignore-alpha.diff
    ];
  });
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
    scenefxPatched
    wlroots_0_20
    libxkbcommon
    libinput
    pixman
    tomlplusplus
    libGL
    libdrm
  ];

  mesonBuildType = "release";

  postInstall = ''
    mkdir -p "$out/share/wayland-sessions"
    cat > "$out/share/wayland-sessions/umbriel.desktop" <<EOF
[Desktop Entry]
Name=Umbriel
Comment=Umbriel Wayland Compositor
Exec=$out/bin/umbriel
Type=Application
DesktopNames=Umbriel
EOF
    wrapProgram $out/bin/umbriel --prefix PATH : ${lib.makeBinPath [ xwayland-satellite ]}
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
