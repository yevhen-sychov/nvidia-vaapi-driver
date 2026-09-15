{
  description = "nvidia-vaapi-driver — VA-API on NVIDIA GPUs, with NVENC encode support";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f (
            import nixpkgs {
              inherit system;
              overlays = [ self.overlays.default ];
            }
          )
        );
    in
    {
      overlays.default = final: prev: {
        nvidia-vaapi-driver = final.callPackage ./nix/package.nix { };
      };

      packages = forAllSystems (pkgs: {
        inherit (pkgs) nvidia-vaapi-driver;
        default = pkgs.nvidia-vaapi-driver;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/shell.nix { };
      });

      checks = forAllSystems (pkgs: {
        # `nix flake check` builds the driver; the meson suite needs a real
        # GPU and so is not part of it (see nix/package.nix).
        build = pkgs.nvidia-vaapi-driver;
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);
    };
}
