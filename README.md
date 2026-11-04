# BLE-to-Matter-over-Thread Gateway

Firmware for an nRF52840 gateway. It brings existing BLE temperature/humidity sensors and BLE thermostats (for example eQ-3 radiator valves) into a Matter smart home over Thread. It is based on the nRF Connect SDK Matter Bridge application and is meant for home use, not certification.

## Layout

| Path | Contents |
| --- | --- |
| [doc/BLE-to-Matter-over-Thread-Gateway.md](doc/BLE-to-Matter-over-Thread-Gateway.md) | Design: architecture, device handling, risks, plan |
| [doc/Toolchain-Setup-VS-Code.md](doc/Toolchain-Setup-VS-Code.md) | Toolchain setup on macOS with VS Code |
| [hello-world/](hello-world/) | Toolchain smoke test (Zephyr `hello_world` for the nRF52840 DK) |
| [spike/](spike/) | Experiments and trial ports, one folder each |
| [spike/matter-bridge-nrf52840/](spike/matter-bridge-nrf52840/) | Trial port of the NCS v3.4.1 Matter Bridge to the `nrf52840dk/nrf52840` board. The board isn't officially supported. |

## Status

- Toolchain: nRF Connect SDK v3.4.1. `hello-world` builds and runs on the nRF52840 DK.
- Matter Bridge on the nRF52840: the port builds, boots on the nRF52840 DK and its Matter shell responds. It hasn't been commissioned yet. The port adds two sets of board files, both copied from the SDK's Matter template sample:
  - `boards/nrf52840dk_nrf52840.{overlay,conf}` for the app, with BLE connections reduced to 3
  - `sysbuild/mcuboot/boards/nrf52840dk_nrf52840.{overlay,conf}` for the bootloader. Without these, MCUboot uses the default flash layout, finds no app and halts silently.

  Memory use for the build with BLE bridging (`CONFIG_BRIDGED_DEVICE_BT=y`):

  | Image | Flash | RAM (reserved at build time) |
  | --- | --- | --- |
  | Bridge app | 843 KB of 975 KB (86%) | 205 KB of 256 KB (78%) |
  | MCUboot | 24 KB of 28 KB (85%) | 18 KB |

  The firmware uses Matter's default test credentials (QR `MT:06PS042C00KA0648G00`, manual code `34970112332`). Run `matter onboardingcodes ble` in the shell to print them.

## Building the spike

Build it from the nRF Connect panel in VS Code, or use `west`:

```sh
west build -b nrf52840dk/nrf52840 --sysbuild -d spike/matter-bridge-nrf52840/build spike/matter-bridge-nrf52840 -- \
  -DCONFIG_BRIDGED_DEVICE_BT=y \
  -DCONFIG_BRIDGE_GENERIC_SWITCH_BRIDGED_DEVICE=n \
  -DCONFIG_BRIDGE_ONOFF_LIGHT_SWITCH_BRIDGED_DEVICE=y
```

If `/opt/nordic` is a symlink to another drive, use the real path for the SDK and toolchain. Otherwise the linker script includes fail to resolve.

To flash, use `--erase` the first time so old settings are cleared:

```sh
west flash -d spike/matter-bridge-nrf52840/build --erase
```

On the command line, `west flash` needs the `device` command for nrfutil. The VS Code extension ships one, so set `NRFUTIL_HOME` to the extension's `platform/nrfutil` folder.
