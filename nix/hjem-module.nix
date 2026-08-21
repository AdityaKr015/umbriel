{
  config,
  pkgs,
  lib,
  ...
}:
let
  inherit (lib.modules) mkIf;
  inherit (lib.options) mkEnableOption mkOption;
  inherit (lib.lists) optional;

  cfg = config.programs.umbriel;
  toml = pkgs.formats.toml { };
in
{
  options.programs.umbriel = {
    enable = mkEnableOption "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

    package = mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The umbriel package to install.";
    };

    settings = mkOption {
      type = lib.types.nullOr toml.type;
      default = null;
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/umbriel/config.toml`.
        Leave null to use the configuration packaged with Umbriel.
        See {file}`examples/config.toml` in the Umbriel repository for every available option.
      '';
      example = lib.literalExpression ''
        general.autostart = [ "noctalia" ];

        keybinds = {
          "Mod+Return" = "spawn:kitty";
          "Mod+Q" = "window-close";
        };
      '';
    };
  };

  config = mkIf cfg.enable {
    packages = optional (cfg.package != null) cfg.package;

    xdg.config.files = mkIf (cfg.settings != null) {
      "umbriel/config.toml".source = toml.generate "umbriel-config.toml" cfg.settings;
    };
  };

  _class = "hjem";
}
