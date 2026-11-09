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

## Status page

Debug builds serve a read-only status page on port 80 of the gateway's Thread address: open `http://[<IPv6>]/` in a browser on the home network, for example `http://[2001:a61:309f:23fc:10ea:7103:5950:8777]/`. The address is in the page's "Thread network" section, and `make netlog` finds the gateway too. The page shows the actuators with their lamps and power, Thread, Matter heap, uptime, firmware, Matter controllers, the TCM and the senders heard; it refreshes every 5 s. `/api/status` returns the same as JSON.

## In the gateway shell: `enocean`

| Command | Purpose |
| --- | --- |
| `enocean info` | TCM version, base ID, receive and send counters |
| `enocean seen` | Senders heard since start: ID, RORG, payload, dBm and hops (direct or through repeaters) of the last telegram, counts, last seen |
| `enocean list` | Bridged actuators: ID, endpoint, kind, light/plug, sender offset, state, power (D2-01) |
| `enocean add <id> eltako\|d2 <offset> light\|plug <name...>` | Bridge an actuator that is taught in to base ID + offset; it keeps its endpoint across restarts |
| `enocean lamps <id> <step offset> <name...>` | For a two-channel Eltako in series-switch mode, added as `eltako` before: one more endpoint per contact, `<name> 1` and `<name> 2` (see below) |
| `enocean remove <id>` | Remove an actuator, and its lamps, from Matter |
| `enocean switch <id> on\|off [1\|2]` | Switch an actuator, or one of its lamps; Matter follows its confirmation |
| `enocean press <offset>` | Send a rocker press from base ID + offset, to no actuator in particular: teach a new sender in, or step a series switch |

- `<id>`: the actuator's EnOcean ID, 8 hex digits, for example `0188EA75`.
- `eltako`: switched like by a rocker (F6). `d2`: D2-01 actuator, addressed commands, state and power read back; its endpoint also has the Electrical Power Measurement cluster.
- `<offset>`: 0..127; the actuator was taught in to the sender ID TCM base ID + offset.
- `light` / `plug`: On/Off Light or On/Off Plug-in Unit in Matter.
- `<name...>`: the starting name in Apple Home; spaces are allowed, umlauts are not (see below).

The actuators in this house (TCM base ID `FFA01D80`):

```
enocean add 0188EA75 eltako 6 light Flur
enocean add 018B3A28 eltako 3 light Kueche rechts
enocean add 0190B727 eltako 7 light Kueche links
enocean add 05017E43 d2 9 plug Kuechenradio
enocean lamps 0190B727 10 Kueche links
enocean lamps 018B3A28 11 Kueche rechts
```

The kitchen actuators are FMS61NP-230V in series-switch mode SS1 (off, lamp 1, lamp 2, both). Their `eltako` sender (base + 7, base + 3) is taught in as central on/off and switches both lamps. For the lamps, a second sender is taught in as series-switch pushbutton: lower dial on the pushbutton symbol, upper dial on LRN, `enocean press <step offset>`, upper dial back to SS1. The gateway then presses that sender until the contacts are as wanted.

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
