{
  config,
  pkgs,
  lib,
  ...
}:
let
  cfg = config.programs.umbriel;
  tomlFormat = pkgs.formats.toml { };

  generateConfig =
    format: name: value:
    if lib.isString value then
      pkgs.writeText name value
    else if builtins.isPath value || lib.isStorePath value then
      value
    else
      format.generate name value;

  generateToml = generateConfig tomlFormat;
in
{
  options.programs.umbriel = {
    enable = lib.mkEnableOption "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The umbriel package to install.";
    };

    settings = lib.mkOption {
      type =
        with lib.types;
        oneOf [
          tomlFormat.type
          str
          path
        ];
      default = { };
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/umbriel/config.toml`.

        Can be written as:
          - A Nix attrset (converted to TOML via nixpkgs' tomlFormat)
          - A raw TOML string
          - A path to a `.toml` file

        See {file}`example.toml` in the Umbriel repository for every available option.
      '';
      example = lib.literalExpression ''
        general = {
          terminal = "kitty";
          autostart = [ "noctalia" ];
        };

        layout.gap = 5;

        input.keyboard.layout = "de";

        keybinds = {
          "Mod+Return" = "spawn:kitty";
          "Mod+Q" = "window-close";
          "Mod+R" = "spawn:noctalia msg panel-toggle launcher";
        };
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = lib.optional (cfg.package != null) cfg.package;

    # Always write the file when enabled so the compositor finds it and can
    # watch the directory. Empty settings still produce a valid empty TOML.
    xdg.configFile."umbriel/config.toml" = {
      source = generateToml "umbriel-config.toml" cfg.settings;
      force = true;
    };
  };

  _class = "homeManager";
}
