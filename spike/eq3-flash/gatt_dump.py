#!/usr/bin/env python3
"""Connect to a thermostat and list its GATT services, characteristics and descriptors.

Read-only: it writes nothing and subscribes to nothing.

    .venv/bin/python gatt_dump.py wohnzimmer        # a name saved by scan.py, or a UUID
"""

import asyncio
import json
import os
import sys

from bleak import BleakClient, BleakScanner

ALIAS_PATH = os.path.expanduser("~/.config/eq3-firmware-mod/devices.json")


def resolve(spec):
    try:
        with open(ALIAS_PATH) as f:
            aliases = {k.lower(): v for k, v in json.load(f).items()}
    except (OSError, ValueError):
        aliases = {}
    return aliases.get(spec.lower(), spec)


async def main(spec):
    address = resolve(spec)
    print(f"Looking for {address} ...")
    dev = await BleakScanner.find_device_by_address(address, timeout=20)
    if dev is None:
        print("Not found. Press a button on the thermostat and try again.")
        return 1
    async with BleakClient(dev, timeout=30) as client:
        print(f"Connected (MTU {client.mtu_size})\n")
        for service in client.services:
            print(f"service {service.uuid}  handle {service.handle}  {service.description}")
            for char in service.characteristics:
                print(f"  char {char.uuid}  handle {char.handle}  [{', '.join(char.properties)}]")
                for desc in char.descriptors:
                    print(f"    desc {desc.uuid}  handle {desc.handle}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(asyncio.run(main(sys.argv[1])))
