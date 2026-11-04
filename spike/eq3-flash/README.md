# eQ-3 custom firmware flashing

Tools to put [eq3-custom-fw](https://github.com/dbuezas/eq3-custom-fw) on eQ-3 CC-RT-BLE thermostats from the Mac's terminal, over Bluetooth, without opening the case. With that firmware the thermostats broadcast their state as BTHome, so the gateway can read them without connecting.

## Setup

```sh
cd spike/eq3-flash
git clone --depth 1 https://github.com/dbuezas/eq3-custom-fw.git
/opt/homebrew/bin/python3 -m venv .venv
.venv/bin/pip install -r eq3-custom-fw/python-scripts/requirements.txt
```

Both `eq3-custom-fw/` and `.venv/` are ignored by git. Tested with commit `13db424` (2026-10-06) of eq3-custom-fw.

## Flashing one thermostat

All thermostats advertise the same name and macOS hides MAC addresses, so they are told apart by signal strength.

1. Turn off the thermostat's PIN if it has one; otherwise flashing stops after 10 s.
2. Hold the thermostat right next to the Mac and save it under a name:
   ```sh
   .venv/bin/python scan.py --save wohnzimmer
   ```
   It saves only if the strongest thermostat is at least 15 dB ahead of the next one. If not, hold it closer, or take the batteries out of the others.
3. Flash it (a few minutes; the connection dropping at the very end means success):
   ```sh
   .venv/bin/python eq3-custom-fw/python-scripts/flash.py wohnzimmer
   ```
4. Set the date with the eQ-3 app. Until then the thermostat does nothing, not even broadcast.
5. `scan.py` now shows it with firmware `custom`.

Back to the stock firmware: `flash.py wohnzimmer --release 1.48`.

Names are stored in `~/.config/eq3-firmware-mod/devices.json`, where `flash.py` also reads them. The UUIDs are specific to this Mac.
