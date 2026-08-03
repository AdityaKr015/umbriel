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
      type = toml.type;
      default = { };
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/umbriel/config.toml`.
        See {file}`example.toml` in the Umbriel repository for every available option.
      '';
      example = lib.literalExpression ''
        general = {
          terminal = "ghostty";
          autostart = [ "noctalia" ];
        };

        keybinds = {
          "Mod+Return" = "spawn-terminal";
          "Mod+Q" = "close";
        };
      '';
    };
  };

  config = mkIf cfg.enable {
    packages = optional (cfg.package != null) cfg.package;

    xdg.config.files = {
      "umbriel/config.toml".source = toml.generate "umbriel-config.toml" cfg.settings;
    };
  };

  _class = "hjem";
}
