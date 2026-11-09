# EnOcean TCM on the nRF52840

First step towards an EnOcean-to-Matter gateway. It is a separate build from the BLE gateway, with no Matter yet. A TCM 310/515 (here on an EnOcean Pi hat) talks ESP3 to the nRF52840 DK over UART1. The firmware logs every radio telegram, decodes the common profiles and has a few shell commands. The point is to see how the EnOcean devices at hand behave before building the real app.

The ESP3 and EEP parsers ([src/esp3.c](src/esp3.c), [src/eep.c](src/eep.c)) are plain C without Zephyr dependencies. They are meant to move into the gateway later.

## Wiring (nRF52840 DK)

| EnOcean Pi (Pi header pin) | Signal | DK |
| --- | --- | --- |
| 1 | 3.3 V | VDD (about 3.0 V on the DK) |
| 6 | GND | GND |
| 8 | module RXD | P1.02 (Arduino D1, UART1 TX) |
| 10 | module TXD | P1.01 (Arduino D0, UART1 RX) |

57600 8N1, no flow control. UART0 stays the console on the J-Link USB port.

## Build, flash, test

```sh
source scripts/ncs-env.sh
west build -b nrf52840dk/nrf52840 -d spike/enocean-tcm/build spike/enocean-tcm
west flash -d spike/enocean-tcm/build

make -C spike/enocean-tcm/tests/host      # parser tests on the Mac
```

Open the console with nRF Terminal or `make term` (115200 baud). At start the firmware reads the TCM's version and base ID:

```
EnOcean spike: listening on uart@40028000
TCM: app 2.x.x.x, API 2.x.x.x, chip ID ..., "GATEWAYCTRL"
TCM: base ID FF80xxxx, 10 base ID changes left
```

If neither line shows, check the wiring (TX and RX swapped is the usual cause) and `tcm stats`.

## Output

One line per telegram: sender ID, RORG, data, status, signal strength, then what it means:

```
0185A1B2 F6 30 st=30 -62dBm | rocker A O pressed
0185A1B2 F6 00 st=20 -62dBm | rocker released
0181C3D4 D5 09 st=00 -71dBm | contact closed
05123456 A5 08280B80 st=00 -55dBm | teach-in A5-02-05, manufacturer 00B
05123456 A5 00008008 st=00 -55dBm | A5-02-05: 19.92 C
```

Decoded so far: rockers (F6-02-01/02), contacts (D5-00-01), temperature (A5-02-01 to A5-02-0B), humidity and temperature (A5-04-01/02), 4BS teach-in, UTE teach-in. Other telegrams are shown raw.

A 4BS sender's profile is learned from its teach-in telegram, which usually comes from a teach-in button on the device. Until then the line shows the data read as A5-02-05 and as A5-04-01, as a hint. Older 4BS devices teach in without a profile; set it by hand with `tcm profile`.

## Shell

| Command | Purpose |
| --- | --- |
| `tcm version` | Read the TCM's firmware version |
| `tcm idbase` | Read the base ID (the gateway would send with it) |
| `tcm devices` | Senders heard, with profile, telegram count, signal and last seen |
| `tcm profile <id> <EEP>` | Set a sender's profile, for example `tcm profile 05123456 A5-04-01` |
| `tcm stats` | ESP3 decoder counters and UART overruns |
| `tcm switch <name> on\|off\|status` | Switch one of the taught-in actuators below; `status` only for D2-01 |

Profiles are kept in RAM only; a restart forgets them.

## Taught-in actuators

The TCM came from an earlier gateway ([enocean-mqtt](https://github.com/BerndDA/enocean-mqtt), on a Pi), so actuators are already taught in to its base ID `FFA01D80`. Do not change the base ID, or they all have to be taught in again. They are listed in `actuators[]` in [src/main.c](src/main.c):

| ID | Name | Kind | Sender |
| --- | --- | --- | --- |
| 0190B727 | kueche_links | Eltako | base + 7 |
| 018B3A28 | kueche_rechts | Eltako | base + 3 |
| 0188EA75 | flur | Eltako | base + 6 |
| 05017E43 | kueche_radio | D2-01 with power metering | base + 9 |

- **Eltako**: switched with one RPS press from the sender, `0x30` = on, `0x10` = off, no release and no optional data. The bytes are identical to what enocean-mqtt sent (see the host tests). The actuator confirms with an F6 telegram: `0x30`/`0x70` = on, `0x10`/`0x50` = off. These confirmations look like rocker presses without a release; the spike shows them as `on`/`off`.
- **D2-01**: command 1 (set output) and command 3 (status query), addressed to the actuator. It answers with command 4 (status) and sends command 7 (power) by itself.

A telegram the gateway sent can come back through a repeater, with the gateway's own sender ID and a repeater count in the status byte (`st=81`). The real gateway has to drop telegrams from base ID .. base ID + 127.
