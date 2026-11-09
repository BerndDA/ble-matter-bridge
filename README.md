# Matter-over-Thread Gateways (BLE and EnOcean)

Firmware for nRF52840 gateways that bring existing devices into a Matter smart home over Thread. There are two separate apps, built from a shared bridge core:

- **BLE gateway** ([firmware/ble-gateway/](firmware/ble-gateway/)): BLE temperature/humidity sensors and BLE thermostats (for example eQ-3 radiator valves).
- **EnOcean gateway** ([firmware/enocean-gateway/](firmware/enocean-gateway/)): EnOcean actuators (Eltako, D2-01) through a TCM 310/515 module on a UART. See [EnOcean gateway](#enocean-gateway).

Both are based on the nRF Connect SDK Matter Bridge application and are meant for home use, not certification. Unless a section says otherwise, it is about the BLE gateway.

## Layout

| Path | Contents |
| --- | --- |
| [firmware/ble-gateway/](firmware/ble-gateway/) | The BLE gateway app |
| [firmware/enocean-gateway/](firmware/enocean-gateway/) | The EnOcean gateway app |
| [firmware/common/](firmware/common/) | Shared by both apps: bridge core, Matter device types, Matter identity from the chip ID, network log, shared Kconfig (`common.cmake`, `Kconfig`) |
| [Makefile](Makefile) | Shortcuts: build, flash, tests, shell, measurements (`make` lists them; `APP=enocean-gateway` for the EnOcean gateway) |
| [scripts/](scripts/) | `ncs-env.sh` sets up the SDK and toolchain; `diag-capture.py` and `diag-summary.py` for measurements |
| [doc/BLE-to-Matter-over-Thread-Gateway.md](doc/BLE-to-Matter-over-Thread-Gateway.md) | Design: architecture, device handling, risks, plan |
| [doc/Toolchain-Setup-VS-Code.md](doc/Toolchain-Setup-VS-Code.md) | Toolchain setup on macOS with VS Code |
| [doc/measurements.md](doc/measurements.md) | Memory, stack and radio measurements |
| [spike/](spike/) | Experiments, one folder each, kept as reference: the toolchain smoke test (`hello-world`), the first bridge port (`matter-bridge-nrf52840`), eQ-3 flashing tools (`eq3-flash`), and an EnOcean TCM on UART for a future EnOcean gateway (`enocean-tcm`) |

## Status

Open tasks and decisions are in [TODO.md](TODO.md).

| Step (from the design doc) | State |
| --- | --- |
| 1. Toolchain and stock bridge on a DK | Done. See [the spike](spike/matter-bridge-nrf52840/). |
| 2. Commission into a controller | Done. Commissioned into Apple Home, with a HomePod as the Thread border router. |
| 3. Advertisement-based sensors | Done for temperature and humidity: pvvx sensors (BTHome v2, unencrypted) are added to Matter automatically and show up in Apple Home. Battery level and low-battery warnings are reported too. |
| 4. One thermostat (connect on demand) | Done: eQ-3 CC-RT-BLE with [custom firmware](spike/eq3-flash/README.md). Values come from its broadcasts; target and Off/Heat are set from Apple Home over a short GATT connection per command. |
| 5. Measure, then scale up or move to the nRF5340 | Measured with sensors and with thermostat commands, see [doc/measurements.md](doc/measurements.md). The nRF52840 has enough headroom, and Thread stays stable. Far sensors are the weak point. |

`firmware/ble-gateway` started as the spike with the parts the gateway doesn't need removed: the simulated devices, the on/off light and switch device types, the smart plug variant and the nRF5340/nRF54 board files. Memory use on the nRF52840 DK:

| Image | Flash | RAM (reserved at build time) |
| --- | --- | --- |
| Gateway app (`make build`) | 866 KB of 975 KB (89 %) | 213 KB of 256 KB (81 %) |
| Gateway app (`make release`) | 783 KB of 975 KB (80 %) | 212 KB of 256 KB (81 %) |
| MCUboot | 24 KB of 28 KB (85 %) | 18 KB |

It takes up to 10 BTHome devices (for example 4 sensors and 4 thermostats) and 20 Matter endpoints.

## Sensors and thermostats

The gateway scans continuously for BTHome v2 advertisements (25 % duty cycle, so Thread keeps most of the radio time). A device heard for the first time is added automatically and stored, so it keeps its endpoints across reboots:
- **Sensors** (pvvx firmware) become a Temperature Sensor and a Humidity Sensor endpoint, named `Sensor XXXX` after the last two bytes of the address.
- **eQ-3 thermostats** (eq3-custom-fw) become one Thermostat endpoint with room temperature, target, Off/Heat, valve opening and battery. A target or mode set from Matter is sent to the thermostat over one short GATT connection (7–14 s until it has it) and also switches it to manual mode. The thermostat's pairing PIN must be off.

  A thermostat renamed with eq3-custom-fw airs `eQ3-<name>`, and is named `<name>` in Matter (`eQ3-Dusche` becomes `Dusche`). The name comes in scan responses, so a new thermostat is added only after up to 20 s of active scanning (`CONFIG_GATEWAY_EQ3_NAME_WAIT_S`). Without a name it gets `Thermostat XXXX`. The gateway then keeps looking every 10 minutes (`CONFIG_GATEWAY_EQ3_NAME_RETRY_MIN`, 20 s active scan each), renames it once the name is heard, and stores the new label. Far thermostats may never be heard well enough for that.

Each device shows manufacturer, model and its Bluetooth address as serial number. A device is reported unreachable after 10 minutes without packets; its last values stay readable, so Apple Home shows "No Response". After a restart, a sensor reports no value until its first packet arrives.

Shell commands (in nRF Terminal, 115200 baud):

| Command | Purpose |
| --- | --- |
| `gateway seen` | All BTHome devices heard, with their latest values (thermostats on a second line) |
| `gateway sensors` | Devices bridged to Matter, with endpoints and reachability |
| `gateway remove <addr>` | Remove a device (for example a neighbour's) and ignore it from now on |
| `gateway unignore <addr>` | Allow an ignored device to be added again |
| `gateway ignored` | List ignored addresses |
| `gateway target <addr> <celsius>` | Set an eQ-3 thermostat's target, for tests (4.5 = off) |
| `gateway pause <s>` | Pause the background scan for measurements (`0` resumes) |
| `gateway identity` | Chip ID, serial number and Matter setup code (also in release builds) |
| `gateway bootloader` | Pro Micro (UF2) only: restart into the UF2 bootloader |

`make term` opens the shell from a terminal.

Settings in [Kconfig](firmware/ble-gateway/Kconfig): `GATEWAY_BTHOME_AUTO_ADD`, `GATEWAY_BTHOME_MAX_SENSORS` (default 10), `GATEWAY_BTHOME_UNREACHABLE_TIMEOUT_S` (default 600), the scan window and interval, and for thermostats `GATEWAY_EQ3_TARGET_SETS_MANUAL`, `GATEWAY_EQ3_ATTEMPTS` and the timeouts.

Parser tests run on the Mac: `make test`.

**Log over the network.** Debug builds send their console output over UDP to a receiver anywhere on the home network, so a gateway on a USB charger can be watched without a cable:

```sh
make netlog                 # finds the gateway, prints its log until Ctrl-C
make netlog SERIAL=9E749B   # with several gateways: pick one by serial number
make netlog DEBUG=1         # include debug lines (much more Thread traffic)
```

[scripts/netlog.py](scripts/netlog.py) finds the gateway through its Matter mDNS records (`dns-sd`, the same path Apple Home uses) and subscribes on UDP port 5140. Nothing needs configuring on either side, and the Mac's address may change. The gateway sends only while a receiver renews its subscription (every 5 s; it lapses after 60 s), info lines and above by default, at most one datagram of 480 bytes per 100 ms. After a gateway restart the log resumes with the next renewal, so the first few seconds of the boot log only show on the console. An idle gateway logs very little. Release builds have no log output. Details: [common/netlog/net_log.h](firmware/common/netlog/net_log.h).

For RAM, stack and radio measurements: `make diag-flash`, then `make measure` (add `THERMOSTAT=<addr>` to send thermostat commands) and `make summary`. See [doc/measurements.md](doc/measurements.md).

## EnOcean gateway

The EnOcean gateway bridges EnOcean actuators that are already taught in to a TCM 310/515 module: each one becomes an On/Off Light or On/Off Plug-in Unit endpoint in Matter. It runs on the nRF52840 DK so far, with the module (for example on an EnOcean Pi hat) on UART1. The protocol work and the wiring are in [spike/enocean-tcm](spike/enocean-tcm/README.md).

| EnOcean Pi (Pi header pin) | DK |
| --- | --- |
| 1 (3.3 V) | VDD |
| 6 (GND) | GND |
| 8 (module RXD) | P1.02 (Arduino D1) |
| 10 (module TXD) | P1.01 (Arduino D0) |

```sh
make flash-erase APP=enocean-gateway   # first time on a board that ran the BLE gateway
make flash APP=enocean-gateway
make test APP=enocean-gateway          # ESP3/EEP parser tests on the Mac
```

Use `flash-erase` when switching a board from one app to the other: otherwise the new app finds the other app's commissioning and bridged devices. Both apps derive the same setup code from the chip ID, but have different product IDs (0x8002 BLE, 0x8003 EnOcean).

Actuators are added in the shell, with the sender ID offset they were taught in with (sender ID = TCM base ID + offset):

```
enocean add 0188EA75 eltako 6 light Flur
enocean add 05017E43 d2 9 plug Kuechenradio
```

| Command | Purpose |
| --- | --- |
| `enocean info` | TCM version, base ID, receive and send counters |
| `enocean seen` | Senders heard since start, with signal strength |
| `enocean list` | Bridged actuators: endpoint, kind, state, power (D2-01) |
| `enocean add <id> eltako\|d2 <offset> light\|plug <name>` | Bridge an actuator; it keeps its endpoint across restarts |
| `enocean lamps <id> <step offset> <name>` | One extra endpoint per contact of a two-channel Eltako in series-switch mode |
| `enocean remove <id>` | Remove it (and its lamps) from Matter |
| `enocean switch <id> on\|off [1\|2]` | Switch it, or one of its lamps, from the shell |

Debug builds have a read-only status page: `http://<Thread extended MAC>.local/` (the hostname Matter registers through the border router) or `http://[<gateway IPv6>]/` from the home network (OpenThread's TCP, so OpenThread is built from source; the page is gzip-compressed into the firmware).

All `make` and shell commands of the EnOcean gateway are collected in [firmware/enocean-gateway/COMMANDS.md](firmware/enocean-gateway/COMMANDS.md).

- **Eltako** actuators are switched like by a rocker (one RPS press from the sender ID: `0x30` on, `0x10` off) and confirm with F6 telegrams. Their state is unknown after a restart until they switch once.
- **Two-channel Eltako** actuators (FMS61NP) confirm each contact on its own: `0x70`/`0x50` is contact 1 on/off, `0x30`/`0x10` contact 2. The actuator's endpoint is on while any contact is on. In series-switch mode (SS1: off, contact 1, contact 2, both) a radio sender can't address one contact, so `enocean lamps` adds one endpoint per contact that the gateway reaches by stepping: it presses a second sender, taught in as series-switch pushbutton (lower dial on the pushbutton symbol), until both contacts are as wanted, about 1 s per step. All on and all off go through the actuator's own (central) sender. The wall switch keeps working, and every endpoint follows it.
- **D2-01** actuators get addressed commands (set output, status query). The gateway asks for their state at start and repeats the query until they answer (30 s, doubling up to 10 min).
- **D2-01** endpoints are also Electrical Sensors: the Electrical Power Measurement cluster reports the load's active power, which the gateway asks for after every status answer (and which actuators set up for it report on their own). Apple Home shows it in the accessory's details, as do Home Assistant and other Matter controllers; `enocean list` shows it too.
- A command from Matter sets the endpoint at once; the actuator's confirmation corrects it if it didn't switch. Telegrams from the gateway's own sender IDs (base ID .. base ID + 127) that come back through repeaters are ignored.
- Never change the TCM's base ID: every actuator taught in to it would have to be taught in again.

## Ready-made firmware

The [releases](https://github.com/BerndDA/ble-matter-bridge/releases) have release builds (no debug logging, watchdog on) for both boards, so no SDK is needed:

- **Pro Micro / nice!nano clone**: double-tap RST to GND, copy `ble-gateway-<version>-promicro.uf2` to the drive that shows up. Needs UF2 bootloader 0.6.1 or later (see [Bootloader version](#pro-micro-nrf52840)).
- **nRF52840 DK**: flash `ble-gateway-<version>-dk.hex` with nRF Connect Programmer or `nrfutil device program --firmware <file> --options chip_erase_mode=ERASE_ALL`.

Every board computes its own setup code (see [Commissioning](#commissioning)). Read it in the shell with `gateway identity`: on the Pro Micro over its USB port, on the DK over the J-Link's serial port, both at 115200 baud.

## Building and flashing

The project is pinned to **nRF Connect SDK v3.4.2**. The build stops with an error on any other SDK version, because the nRF52840 port has only been checked on this one.

In VS Code, `firmware/ble-gateway` and `firmware/enocean-gateway` appear in the nRF Connect panel. Add a build configuration for board `nrf52840dk/nrf52840` with sysbuild.

From a terminal:

```sh
make flash          # build and flash; keeps commissioning and stored devices
make flash-erase    # full erase: the gateway must be commissioned again
make release        # release configuration (prj_release.conf) into build-release
```

Add `BOARD=promicro` or `BOARD=promicro-uf2` to any target for the Pro Micro nRF52840. See [Pro Micro nRF52840](#pro-micro-nrf52840). Add `APP=enocean-gateway` for the EnOcean gateway (DK only so far). `make test` runs the host tests of the chosen app.

The targets set up the SDK themselves through `scripts/ncs-env.sh`. For `west` by hand, run `source scripts/ncs-env.sh` first. The script uses the external drive if it's mounted and `/opt/nordic` otherwise. Set `NCS_ROOT` to use a different location. If `/opt/nordic` is a symlink, point `NCS_ROOT` at the real path instead, because the symlink breaks some linker script includes. It also points `NRFUTIL_HOME` at the nrfutil that ships with the VS Code extension, which `west flash` needs.

### Commissioning

Every gateway has its own Matter identity, so several can be in the same home. The firmware derives it at boot from the chip's unique ID (FICR DEVICEID), on every board and in every build: serial number `BLEGW-<chip ID>`, and discriminator, setup code, SPAKE2+ salt and rotating device ID hashed from the chip ID ([common/identity/](firmware/common/identity/chip_id_identity.h)). One firmware image thus works for every board, and the same board always gets the same codes, also after a re-flash. Certificates, vendor and product still come from the factory data.

To get a board's setup code:

- `matter onboardingcodes ble` in the gateway's shell, or
- `make identity` (DK, chip ID read over the J-Link), `make identity BOARD=promicro-uf2` (board on its UF2 drive), or `make identity CHIP_ID=<16 hex digits>` for any board. It also writes a QR code image to `<build folder>/identity/<chip ID>/factory_data.png`. `make flash BOARD=promicro-uf2` prints the code as well.

The derivation isn't secret, just like a code printed on a device: anyone holding the board can read its chip ID. Apple Home shows an "Uncertified Accessory" warning; choose **Add Anyway**. To factory-reset the gateway, hold **Button 1** on the DK for about 6 seconds, or run `matter device factoryreset` in the shell. A gateway commissioned with older firmware that had other codes keeps working, but must be factory-reset and removed from Apple Home (see below) before it can be added again with its new codes.

**Removing and adding again.** iOS adds the gateway to two Matter fabrics: the Home app's and its own. Removing it in the Home app only removes the first one. The gateway then stays commissioned and doesn't advertise, so it can't be added again. Also remove it under **Settings → General → Matter Accessories** on the iPhone. With its last fabric gone, the gateway erases itself and restarts, ready to be added again. A factory reset does the same.

### Pro Micro nRF52840

The Pro Micro (also sold as nice!nano clone or "SuperMini") is a second option next to the DK. It runs the same firmware, and there are two ways to flash it:

- **`BOARD=promicro-uf2`**: keeps the UF2 bootloader the board ships with. You flash over USB by copying a file. No wiring needed.
- **`BOARD=promicro`**: flashed over SWD with the DK as programmer. This erases the UF2 bootloader, and gives a little more flash and a real full erase.

| | DK | Pro Micro, UF2 | Pro Micro, SWD |
| --- | --- | --- | --- |
| Flashing | J-Link on board | `.uf2` file over USB | SWD from the DK's Debug Out |
| Bootloader | MCUboot, Matter OTA prepared | Adafruit UF2, no OTA | none, no OTA |
| Flash (debug / release) | 866 / 783 KB of 952 KB | 845 / 769 KB of 928 KB | 845 / 769 KB of 988 KB |
| Full erase | `make flash-erase` | not possible over UF2; use the factory reset | `make flash-erase BOARD=promicro` |
| RAM (reserved at build time) | 213 KB | 218 KB (USB stack) | 218 KB |
| Shell | UART through the J-Link | USB CDC on the board's own USB port | same |
| Factory reset | hold Button 1 for 6 s | optional button between P1.06 and GND, held for 6 s, or `matter device factoryreset` in the shell | same |
| Status LED | LED1 | the LED on P0.15 | same |
| 32 kHz clock | crystal | internal RC oscillator, because many clones have no crystal | same |

There is no OTA on either Pro Micro variant: without external flash there is no room for a second image slot.

**UF2 (`BOARD=promicro-uf2`).** Run:

```sh
make flash BOARD=promicro-uf2   # builds ble-gateway.uf2 and copies it to the drive; the board restarts
make term BOARD=promicro-uf2    # shell over the Pro Micro's USB port
```

If the board isn't on its UF2 drive yet, `make flash` sends `gateway bootloader` over the USB shell first (`make term` must be closed) and waits for the drive. The very first time, or with firmware older than that command, short RST to GND twice quickly instead; the board then shows up as a USB drive.

`scripts/make-uf2.py` merges the app and the Matter factory data into one `ble-gateway.uf2`. The settings live in an area the bootloader never writes, so they survive every flash. That's also why `flash-erase` doesn't work here. To start from scratch, use `matter device factoryreset` or the button instead.

**Bootloader version.** The gateway needs UF2 bootloader 0.6.1 or later; check the first line of `INFO_UF2.TXT` on the drive. Older versions, such as the 0.6.0 that some clones ship with, only write up to 0xAD000 and would silently drop the end of the gateway and its factory data. Update by copying `update-nice_nano_bootloader-<version>_nosd.uf2` from the [Adafruit releases](https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases) to the drive; it keeps the SoftDevice, which the first `ble-gateway.uf2` then overwrites. Tested: 0.6.0 updated to 0.11.0 this way, and the gateway runs on it. Details in [spike/uf2-bootloader-dk](spike/uf2-bootloader-dk/README.md).

**SWD (`BOARD=promicro`).** The DK flashes the Pro Micro through its **Debug Out** header (P20). Connect the header to the SWD pads on the back of the Pro Micro: SWDIO, SWDCLK, GND, and VTref (pin 1) to the Pro Micro's 3.3 V. The DK switches to the external target when it sees a voltage on VTref. Power the Pro Micro over its own USB port, which also carries the shell. The first flash must be a full erase, because the UF2 bootloader's data sits where this variant keeps its settings:

```sh
make flash-erase BOARD=promicro   # once; afterwards `make flash BOARD=promicro`
make term BOARD=promicro
```

To go back to UF2 afterwards, flash the board's UF2 bootloader hex over SWD again.

The build folders end in `-promicro` or `-promicro-uf2`. `make measure` only finds the DK's port.

The DK and the Pro Micro have different Matter credentials (see [Commissioning](#commissioning)), so both can be commissioned at once.

## Notes on the nRF52840 port

The nRF52840 DK isn't a supported board for the SDK's Matter Bridge. Two sets of board files in each app folder, both copied from the SDK's Matter template sample, make it work:
- `boards/nrf52840dk_nrf52840.{overlay,conf}`: the Matter flash layout with the 8 MB external flash, and BLE connections reduced to 3.
- `sysbuild/mcuboot/boards/nrf52840dk_nrf52840.{overlay,conf}`: the same layout for the bootloader. Without these, MCUboot looks for the app at the wrong address and halts silently.

The Pro Micro uses the board definition that ships with Zephyr: `promicro_nrf52840/nrf52840` for SWD and `promicro_nrf52840/nrf52840/uf2` for UF2. The project adds:
- `boards/promicro_nrf52840_nrf52840.{overlay,conf}` (SWD): app from 0x0, with factory data and settings at the same addresses as on the DK.
- `boards/promicro_nrf52840_nrf52840_uf2.{overlay,conf}` (UF2): app from 0x1000 after the MBR, factory data at 0xe9000, settings at 0xea000. The bootloader only writes up to 0xea000, and keeps the 40 KB above that for the app's data. The overlay has the full map.
- `boards/promicro_nrf52840_common.dtsi`: the function button and the disabled peripherals, used by both.

`Kconfig.sysbuild` turns off MCUboot and Matter OTA for the Pro Micro.

MCUboot is currently signed with the SDK's default debug key. Generate a project key before relying on firmware updates.
