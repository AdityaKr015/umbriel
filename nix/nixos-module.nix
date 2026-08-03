{
  config,
  pkgs,
  lib,
  ...
}:
let
  cfg = config.programs.umbriel;
in
{
  options.programs.umbriel = {
    enable = lib.mkEnableOption "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The umbriel package to install.";
    };
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        hardware.graphics.enable = lib.mkDefault true;

        assertions = [
          {
            assertion = cfg.package != null;
            message = "programs.umbriel.package cannot be null when programs.umbriel.enable is true";
          }
        ];
      }

      (lib.mkIf (cfg.package != null) {
        environment.systemPackages = [
          cfg.package
          # So `just debug` / meson outside `nix develop` can find headers via pkg-config.
          pkgs.tomlplusplus
        ];

        # Required for greetd / noctalia-greeter to discover the session (Name=Umbriel).
        # Plain systemPackages .desktop files are not enough; NixOS aggregates via this.
        services.displayManager.sessionPackages = [ cfg.package ];
      })
    ]
  );
}
