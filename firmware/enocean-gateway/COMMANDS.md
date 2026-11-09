# EnOcean gateway: commands

Every command for building, flashing and running the EnOcean gateway, in one place. The background (wiring, how the actuators are switched) is in the [README](../../README.md#enocean-gateway).

## On the Mac (`make`)

Run from the repository root. `APP=enocean-gateway` is required on every call; without it, `make` builds the BLE gateway.

| Command | Purpose |
| --- | --- |
| `make build APP=enocean-gateway` | Build (incremental) into `firmware/enocean-gateway/build` |
| `make flash APP=enocean-gateway` | Build and flash; keeps commissioning and bridged actuators |
| `make flash-erase APP=enocean-gateway` | Build and flash with a full erase. Use it the first time on a board that ran the BLE gateway; the gateway must be commissioned again |
| `make release APP=enocean-gateway` | Release build (no logging, watchdog, reset on fatal error) into `build-release` |
| `make test APP=enocean-gateway` | ESP3/EEP parser tests on the Mac |
| `make term` | Open the gateway shell (115200 baud, Ctrl+] quits); close nRF Terminal first |
| `make identity APP=enocean-gateway` | Show the board's Matter setup code |
| `make netlog` | Show the log over the network (debug builds); `SERIAL=<part>`, `DEBUG=1`, `HOST=<IPv6>` |
| `make zap APP=enocean-gateway` | Regenerate the data model code after editing `src/default_zap/bridge.zap` |
| `make clean APP=enocean-gateway` | Delete the build folders and the test build |

Only the nRF52840 DK is supported so far, so leave `BOARD` at its default. The `diag`, `measure` and `summary` targets belong to the BLE gateway.

## In the gateway shell: `enocean`

| Command | Purpose |
| --- | --- |
| `enocean info` | TCM version, base ID, receive and send counters |
| `enocean seen` | Senders heard since start: ID, RORG, payload, dBm, count, last seen |
| `enocean list` | Bridged actuators: ID, endpoint, kind, light/plug, sender offset, state, power (D2-01) |
| `enocean add <id> eltako\|d2 <offset> light\|plug <name...>` | Bridge an actuator that is taught in to base ID + offset; it keeps its endpoint across restarts |
| `enocean remove <id>` | Remove an actuator from Matter |
| `enocean switch <id> on\|off` | Switch an actuator; Matter follows its confirmation |

- `<id>`: the actuator's EnOcean ID, 8 hex digits, for example `0188EA75`.
- `eltako`: switched like by a rocker (F6). `d2`: D2-01 actuator, addressed commands, state and power read back.
- `<offset>`: 0..127; the actuator was taught in to the sender ID TCM base ID + offset.
- `light` / `plug`: On/Off Light or On/Off Plug-in Unit in Matter.
- `<name...>`: the starting name in Apple Home; spaces are allowed, umlauts are not (see below).

The actuators in this house (TCM base ID `FFA01D80`):

```
enocean add 0188EA75 eltako 6 light Flur
enocean add 018B3A28 eltako 3 light Kueche rechts
enocean add 0190B727 eltako 7 light Kueche links
enocean add 05017E43 d2 9 plug Kuechenradio
```

The shell accepts only ASCII: umlauts are dropped (`Küche` arrives as `Kche`). The name is only the starting name in Apple Home; rename the accessory there to get the umlauts.

Never change the TCM's base ID: every actuator taught in to it would have to be taught in again, and the module allows only 10 changes.

## In the gateway shell: Matter

| Command | Purpose |
| --- | --- |
| `matter onboardingcodes ble` | Setup code and QR code for commissioning |
| `matter config pincode` / `matter config discriminator` | Setup PIN and discriminator |
| `matter device factoryreset` | Factory reset (same as holding Button 1 for 6 s); removes commissioning and bridged actuators |
| `matter device opencommissioningwindow` | Open the commissioning window again |
| `matter dns browse operational` | List Matter nodes on the network |
| `matter stat peak` | Peak usage of system resources |
| `matter version` | Software version |
| `matter help` | All Matter shell commands |
| `matter_settings current` / `free` / `peak` | Usage of the persistent storage |

`help` lists all top-level commands; `<command> -h` shows the subcommands. The OpenThread shell (`ot ...`) is not built in.
