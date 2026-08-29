<!--
SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

Integrated Wi-Fi sensing and communication for soil moisture monitoring using channel state information (CSI).

## Structure

```text
.
├── LICENSES/   # Project licenses
├── modules/    # Project-specific Zephyr modules
├── stations/   # Zephyr applications
├── west.yml    # West manifest
├── flake.nix   # Nix development environment
├── flake.lock  # Locked Nix dependencies
└── README.md
```

## Stations

The `stations/` directory contains the code and configuration for each device role. Each application is built independently and targets a specific board.

### Collector

The `collector` station acquires Wi-Fi CSI and reference sensor samples. The data is sent to the `relay` station through Wi-Fi.

The board should have an RF switch on the Wi-Fi output. The target tested for this role is `xiao_esp32c6/esp32c6/hpcore`.

### Relay

The `relay` station aggregates data from multiple `collector` stations and sends them via LoRa to the `hub` station. The data received is also saved on a local filesystem (by default, to a microSD card on the targeted board).

The `relay` acts as a Wi-Fi access point (AP) for the `collector` stations, generating the Wi-Fi transmissions necessary to estimate CSI.

The board should have a LoRa radio. The target tested for this role is `ttgo_lora32/esp32/procpu`.

### Hub

The `hub` station connects to a Wi-Fi network with internet access. It receives LoRa transmissions from the `relay` station and sends them to a Google Apps Script for formatting and storage.

The board should have a LoRa radio. The target tested for this role is `ttgo_lora32/esp32/procpu`.

## Modules

The `modules/` directory contains project-specific Zephyr modules shared by the applications. These provide reusable drivers, libraries, and other functionality that is not part of the applications themselves.

## Install

### Nix

Install Nix using the instructions on the [official Nix website](https://nixos.org/download/).

On Linux, Nix can also be installed through the system's package manager. For example:

```sh
# Debian/Ubuntu
sudo apt install nix

# Fedora
sudo dnf install nix

# Arch Linux
sudo pacman -S nix
```

Refer to your distribution's documentation for any additional configuration required.

### Development environment

Enter the project's development environment from the repository root:

```sh
nix develop
```

This provides the tools required to build and work with the Zephyr applications.

## Build

Build an application by changing into its directory and running the West build command:

```sh
west build -b <board_target> -p always
```

See [Stations](#stations) for the `<board_targets>` used by each project and their requirements.

The build output is generated in the `build/` directory. Following builds may use the same command with no arguments:

```sh
west build
```

## Flash

With a board connected, flash the application using:

```sh
west flash
```

This command will look for a build directory in the current application directory (if any). For a specific build directory run:

```sh
west flash -d <build_dir>
```

## Monitor

Monitor the application's serial output with:

```sh
west espressif monitor
```

Use the monitor command appropriate for the target board when necessary.

To exit the monitor, press:

```text
Ctrl+]
```

