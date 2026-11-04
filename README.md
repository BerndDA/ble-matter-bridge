# BLE-to-Matter-over-Thread Gateway

Firmware for an nRF52840 gateway. It brings existing BLE temperature/humidity sensors and BLE thermostats (for example eQ-3 radiator valves) into a Matter smart home over Thread. It is based on the nRF Connect SDK Matter Bridge application and is meant for home use, not certification.

## Layout

| Path | Contents |
| --- | --- |
| [firmware/gateway/](firmware/gateway/) | The gateway application |
| [scripts/ncs-env.sh](scripts/ncs-env.sh) | Sets up the SDK and toolchain for terminal builds |
| [doc/BLE-to-Matter-over-Thread-Gateway.md](doc/BLE-to-Matter-over-Thread-Gateway.md) | Design: architecture, device handling, risks, plan |
| [doc/Toolchain-Setup-VS-Code.md](doc/Toolchain-Setup-VS-Code.md) | Toolchain setup on macOS with VS Code |
| [hello-world/](hello-world/) | Toolchain smoke test (Zephyr `hello_world` for the nRF52840 DK) |
| [spike/](spike/) | Experiments and trial ports, one folder each. Kept as reference, not maintained. |

## Status

| Step (from the design doc) | State |
| --- | --- |
| 1. Toolchain and stock bridge on a DK | Done. See [the spike](spike/matter-bridge-nrf52840/). |
| 2. Commission into a controller | Done. Commissioned into Apple Home, with a HomePod as the Thread border router. |
| 3. Advertisement-based sensors | In progress. Target: thermometers running pvvx firmware that advertise BTHome v2, unencrypted. |
| 4. One thermostat (connect on demand) | Not started |
| 5. Measure, then scale up or move to the nRF5340 | Not started |

`firmware/gateway` is the spike with the parts the gateway doesn't need removed: the simulated devices, the on/off light and switch device types, the smart plug variant and the nRF5340/nRF54 board files. Memory use on the nRF52840 DK:

| Image | Flash | RAM (reserved at build time) |
| --- | --- | --- |
| Gateway app | 838 KB of 975 KB (86%) | 205 KB of 256 KB (78%) |
| MCUboot | 24 KB of 28 KB (85%) | 18 KB |

## Building and flashing

The project is pinned to **nRF Connect SDK v3.4.1**. The build stops with an error on any other SDK version, because the nRF52840 port has only been checked on this one.

In VS Code, `firmware/gateway` appears in the nRF Connect panel. Add a build configuration for board `nrf52840dk/nrf52840` with sysbuild.

From a terminal:

```sh
source scripts/ncs-env.sh
west build -b nrf52840dk/nrf52840 --sysbuild -d firmware/gateway/build firmware/gateway
west flash -d firmware/gateway/build            # keeps commissioning and stored devices
west flash -d firmware/gateway/build --erase    # full erase: the gateway must be commissioned again
```

The script uses the real install path on the external drive. The `/opt/nordic` symlink breaks some linker script includes. It also points `NRFUTIL_HOME` at the nrfutil that ships with the VS Code extension, which `west flash` needs.

### Commissioning

The firmware uses Matter's default test credentials:
- QR code: `MT:06PS042C00KA0648G00`
- Manual code: `34970112332`

Run `matter onboardingcodes ble` in the shell to print them. Apple Home shows an "Uncertified Accessory" warning; choose **Add Anyway**. To factory-reset the gateway, hold **Button 1** on the DK for about 6 seconds.

## Notes on the nRF52840 port

The nRF52840 DK isn't a supported board for the SDK's Matter Bridge. Two sets of board files, both copied from the SDK's Matter template sample, make it work:
- `boards/nrf52840dk_nrf52840.{overlay,conf}`: the Matter flash layout with the 8 MB external flash, and BLE connections reduced to 3.
- `sysbuild/mcuboot/boards/nrf52840dk_nrf52840.{overlay,conf}`: the same layout for the bootloader. Without these, MCUboot looks for the app at the wrong address and halts silently.

MCUboot is currently signed with the SDK's default debug key. Generate a project key before relying on firmware updates.
