# SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

# SPDX-License-Identifier: MIT

{
  description = "Integrated Wi-Fi sensing and communication for soil moisture monitoring";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    zephyr-rtos = {
      url = "github:nix-community/zephyr-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      nixpkgs,
      self,
      zephyr-rtos,
      ...
    }:
    let
      inherit (nixpkgs)
        lib
        ;

      forEachSystem =
        fn: (lib.genAttrs lib.systems.flakeExposed) (system: fn system nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forEachSystem (
        system: pkgs:
        let
          zephyr = zephyr-rtos.packages.${system};
        in
        {
          default = pkgs.mkShellNoCC {
            packages = with pkgs; [
              (zephyr.sdk.override {
                targets = [
                  "xtensa-espressif_esp32_zephyr-elf"
                  "xtensa-espressif_esp32s3_zephyr-elf"
                  "riscv64-zephyr-elf"
                ];
              })
              zephyr.pythonEnv
              zephyr.hosttools
              cmake
              ninja
              esptool
            ];

            shellHook = ''
              source <(west completion bash)
            '';

            PYTHONPATH = "${zephyr.pythonEnv}/${zephyr.pythonEnv.sitePackages}";
          };
        }
      );
    };
}
