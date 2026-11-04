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

Open tasks and decisions are in [TODO.md](TODO.md).

| Step (from the design doc) | State |
| --- | --- |
| 1. Toolchain and stock bridge on a DK | Done. See [the spike](spike/matter-bridge-nrf52840/). |
| 2. Commission into a controller | Done. Commissioned into Apple Home, with a HomePod as the Thread border router. |
| 3. Advertisement-based sensors | Done for temperature and humidity: pvvx sensors (BTHome v2, unencrypted) are added to Matter automatically and show up in Apple Home. Battery is still missing. |
| 4. One thermostat (connect on demand) | Not started |
| 5. Measure, then scale up or move to the nRF5340 | First measurement done with two sensors, see [doc/measurements.md](doc/measurements.md). The nRF52840 has enough headroom, and Thread stays stable while scanning. Far sensors are the weak point. To be measured again after step 4. |

`firmware/gateway` is the spike with the parts the gateway doesn't need removed: the simulated devices, the on/off light and switch device types, the smart plug variant and the nRF5340/nRF54 board files. Memory use on the nRF52840 DK:

| Image | Flash | RAM (reserved at build time) |
| --- | --- | --- |
| Gateway app | 844 KB of 975 KB (87%) | 208 KB of 256 KB (79%) |
| MCUboot | 24 KB of 28 KB (85%) | 18 KB |

## Sensors

The gateway scans continuously for BTHome v2 advertisements (25 % duty cycle, so Thread keeps most of the radio time). A sensor heard for the first time is added as a Temperature Sensor and a Humidity Sensor endpoint, named `Sensor XXXX` after the last two bytes of its address. Added sensors are stored and keep their endpoints across reboots. A sensor is reported unreachable after 10 minutes without packets.

Shell commands (in nRF Terminal, 115200 baud):

| Command | Purpose |
| --- | --- |
| `gateway seen` | All BTHome sensors heard, with their latest values |
| `gateway sensors` | Sensors bridged to Matter, with endpoints and reachability |
| `gateway remove <addr>` | Remove a sensor (for example a neighbour's) and ignore it from now on |
| `gateway unignore <addr>` | Allow an ignored sensor to be added again |
| `gateway ignored` | List ignored addresses |
| `gateway pause <s>` | Pause the background scan for measurements (`0` resumes) |

Settings in [Kconfig](firmware/gateway/Kconfig): `GATEWAY_BTHOME_AUTO_ADD`, `GATEWAY_BTHOME_MAX_SENSORS` (default 6), `GATEWAY_BTHOME_UNREACHABLE_TIMEOUT_S` (default 600), and the scan window and interval.

Parser tests run on the Mac: `make -C firmware/gateway/tests/host/bthome`.

For RAM, stack and radio measurements, use the diagnostics build (`-DEXTRA_CONF_FILE=diag.conf`) with `scripts/diag-capture.py` and `scripts/diag-summary.py`. See [doc/measurements.md](doc/measurements.md).

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

The script uses the external drive if it's mounted and `/opt/nordic` otherwise. Set `NCS_ROOT` to use a different location. If `/opt/nordic` is a symlink, point `NCS_ROOT` at the real path instead, because the symlink breaks some linker script includes. It also points `NRFUTIL_HOME` at the nrfutil that ships with the VS Code extension, which `west flash` needs.

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
