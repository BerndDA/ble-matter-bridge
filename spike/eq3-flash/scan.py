#!/usr/bin/env python3
"""List nearby eQ-3 thermostats with signal strength, to tell identical ones apart.

macOS hides Bluetooth MAC addresses and names every device by its own UUID, and all
stock thermostats advertise the same name (CC-RT-BLE). Hold the thermostat you want
right next to the Mac: it has by far the strongest signal.

    .venv/bin/python scan.py                    # list, strongest first
    .venv/bin/python scan.py --save wohnzimmer  # also save the strongest under a name

A saved name works in place of the UUID with flash.py, e.g.
    .venv/bin/python eq3-custom-fw/python-scripts/flash.py wohnzimmer

Names are stored where flash.py reads them: ~/.config/eq3-firmware-mod/devices.json
"""

import argparse
import asyncio
import json
import os
import statistics

from bleak import BleakScanner

ALIAS_PATH = os.path.expanduser("~/.config/eq3-firmware-mod/devices.json")
ADV_NAME = "CC-RT-BLE"
ADV_PREFIX = "eQ3-"
BTHOME_UUID = "0000fcd2-0000-1000-8000-00805f9b34fb"
CLEAR_MARGIN_DB = 15  # --save only when the strongest is this much ahead of the next one


async def scan(seconds):
    seen = {}  # address -> {"name": str, "rssi": [int], "bthome": bool}

    def cb(dev, adv):
        name = adv.local_name or dev.name or ""
        bthome = BTHOME_UUID in (adv.service_data or {})
        # BTHome alone also matches other sensors (e.g. pvvx "ATC_..."), so only when no other name is aired.
        if name == ADV_NAME or name.startswith(ADV_PREFIX) or (bthome and not name):
            e = seen.setdefault(dev.address, {"name": name, "rssi": [], "bthome": False})
            e["name"] = name or e["name"]
            e["rssi"].append(adv.rssi)
            e["bthome"] |= bthome

    scanner = BleakScanner(cb)
    await scanner.start()
    try:
        await asyncio.sleep(seconds)
    finally:
        await scanner.stop()
    return seen


def load_aliases():
    try:
        with open(ALIAS_PATH) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=15, help="scan time (default 15)")
    ap.add_argument("--save", metavar="NAME", help="save the strongest thermostat under NAME")
    args = ap.parse_args()

    print(f"Scanning for {args.seconds:.0f} s ...")
    seen = asyncio.run(scan(args.seconds))
    # Drop devices that aired BTHome without a name first and a foreign name later.
    seen = {a: e for a, e in seen.items()
            if not e["name"] or e["name"] == ADV_NAME or e["name"].startswith(ADV_PREFIX)}
    if not seen:
        print("Nothing found. Scan again (macOS misses adverts), or press a button on the thermostat.")
        return 1

    names = {v.lower(): k for k, v in load_aliases().items()}
    rows = sorted(seen.items(), key=lambda kv: -statistics.median(kv[1]["rssi"]))
    print(f"{'UUID':38} {'Name':14} {'RSSI':>5} {'Adverts':>7}  Firmware   Saved as")
    for addr, e in rows:
        fw = "custom" if e["bthome"] else "stock?"
        print(f"{addr:38} {e['name'] or '-':14} {statistics.median(e['rssi']):5.0f} {len(e['rssi']):7}"
              f"  {fw:9}  {names.get(addr.lower(), '')}")

    if args.save:
        best_addr, best = rows[0]
        best_rssi = statistics.median(best["rssi"])
        if len(rows) > 1:
            margin = best_rssi - statistics.median(rows[1][1]["rssi"])
            if margin < CLEAR_MARGIN_DB:
                print(f"\nNot saved: the strongest is only {margin:.0f} dB ahead of the next one "
                      f"(need {CLEAR_MARGIN_DB}). Hold it closer to the Mac and scan again.")
                return 1
        aliases = load_aliases()
        aliases[args.save] = best_addr
        os.makedirs(os.path.dirname(ALIAS_PATH), exist_ok=True)
        with open(ALIAS_PATH, "w") as f:
            json.dump(aliases, f, indent=2)
        print(f"\nSaved {best_addr} ({best_rssi:.0f} dBm) as \"{args.save}\" in {ALIAS_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
