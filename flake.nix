{
  description = "Umbriel, a Wayland compositor built on wlroots and SceneFX.";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.xz";
    scenefx = {
      url = "github:noctalia-dev/scenefx/umbriel";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      scenefx,
    }:
    let
      inherit (nixpkgs.lib) genAttrs getExe;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forEachSystem =
        perSystem:
        genAttrs systems (
          system:
          let
            pkgs = nixpkgs.legacyPackages.${system};
          in
          perSystem {
            inherit pkgs system;
            scenefxPkg = scenefx.packages.${system}.default;
          }
        );
    in
    {
      overlays.default = final: prev: {
        umbriel = final.callPackage ./nix/package.nix {
          scenefx = scenefx.packages.${final.stdenv.hostPlatform.system}.default;
        };
      };

      packages = forEachSystem (
        { pkgs, scenefxPkg, ... }:
        {
          default = pkgs.callPackage ./nix/package.nix { scenefx = scenefxPkg; };
        }
      );

      devShells = forEachSystem (
        { pkgs, system, ... }:
        {
          default = pkgs.callPackage ./nix/devshell.nix {
            umbriel = self.packages.${system}.default;
          };
        }
      );

      apps = forEachSystem (
        { system, ... }:
        {
          default = {
            type = "app";
            program = getExe self.packages.${system}.default;
          };
        }
      );

      homeModules.default =
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/home-module.nix ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      hjemModules.default =
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/hjem-module.nix ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };

      nixosModules.default =
        { pkgs, lib, ... }:
        {
          imports = [ ./nix/nixos-module.nix ];
          programs.umbriel.package = lib.mkDefault self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
    };
}
