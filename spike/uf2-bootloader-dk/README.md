# UF2 bootloader on the nRF52840 DK

Test bed for `BOARD=promicro-uf2` without a Pro Micro: the DK gets the Adafruit UF2 bootloader, and the gateway is flashed over the DK's **nRF USB** port (J3, on the long edge), exactly like on the Pro Micro.

Tested 2026-11-05 with bootloader 0.11.0, both without and with SoftDevice. In both cases `make flash BOARD=promicro-uf2` worked unchanged: the app in flash matches `zephyr.hex`, the factory data matches except the zeroed DAC key (moved into secure storage on first boot), Matter starts, and the settings at 0xea000 survive a second UF2 flash.

## Flashing the bootloader

Adafruit publishes only the SoftDevice variant for the DK (`pca10056`). `strip-sd.py` removes the SoftDevice and keeps MBR, bootloader, bootloader settings and UICR, which equals their `_nosd.hex` build. `pca10056_bootloader-0.11.0_nosd.hex` is that result.

```sh
source ../../scripts/ncs-env.sh
nrfjprog --eraseall
nrfjprog --program pca10056_bootloader-0.11.0_nosd.hex --verify --reset
```

To test the SoftDevice case (like most nice!nano-style clones), flash the release file instead:

```sh
gh release download 0.11.0 -R adafruit/Adafruit_nRF52_Bootloader -p 'pca10056_bootloader-0.11.0_s140_6.1.1.hex'
```

The first `gateway.uf2` overwrites the SoftDevice; the bootloader then no longer finds it and starts the app at 0x1000.

With no app on the chip the bootloader stays in UF2 mode and the `NRF52BOOT` drive mounts. Otherwise double-press RESET, hold Button 1 while pressing RESET, or without touching the board:

```sh
nrfjprog --memwr 0x4000051C --val 0x57 && nrfjprog --reset   # GPREGRET = enter UF2
```

Then `make flash BOARD=promicro-uf2` from the repo root. `make flash-erase` restores the DK.

## Checking a bootloader

`INFO_UF2.TXT` on the drive tells whether a board's bootloader fits the `promicro-uf2` layout: the first line must be `UF2 Bootloader x.y.z` (Adafruit or a fork such as nice!nano). `SoftDevice:` may say `not found` or `S140 6.1.1`. An old version can be updated over USB with the release's `update-<board>_bootloader-<ver>_nosd.uf2`.

## Shell over USB

`sh.py PORT 'cmd' ...` sends shell commands over USB CDC. The boot log waits in a 1 KB buffer until a host opens the port with DTR set, and the shell's TX ring buffer is only 8 bytes, so drain the backlog before sending commands, or they queue behind it. `make term` reads continuously and isn't affected.

`src/` (ignored) is a clone of the bootloader source at tag 0.11.0, for reading.
