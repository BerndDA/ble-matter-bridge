#!/usr/bin/env python3
"""Build one .uf2 from the gateway's hex files and optionally copy it to a UF2 bootloader drive.

For the Pro Micro nRF52840 with the factory UF2 bootloader (make ... BOARD=promicro-uf2).
The app and the Matter factory data go into one .uf2. The bootloader only writes
0x1000-0xea000, so everything must lie in that range (see
firmware/gateway/boards/promicro_nrf52840_nrf52840_uf2.overlay).

Per-board identity (--identity BUILD_DIR): instead of the build's factory data,
which every board would share, the factory data is generated for the board on the
UF2 drive. Its chip ID (FICR DEVICEID, which the bootloader reports as its USB
serial number) gives serial number, discriminator, setup code, SPAKE2+ salt and
rotating device ID, always the same for the same board (see derive_identity).
All other values come from the build's own generator command, so they follow Kconfig.

Usage:
  make-uf2.py -o gateway.uf2 [--deploy] app.hex factory_data.hex
  make-uf2.py -o gateway.uf2 --deploy --identity BUILD_DIR app.hex
  make-uf2.py --identity BUILD_DIR --chip-id 54932B32846202D7 --show   (codes of a board, no drive needed)
"""
import argparse
import base64
import glob
import hashlib
import os
import plistlib
import re
import shlex
import subprocess
import sys
import tempfile

from intelhex import IntelHex

FAMILY_NRF52840 = "0xADA52840"
# Adafruit_nRF52_Bootloader: USER_FLASH_START = MBR size, USER_FLASH_END = bootloader - 40 KB
UF2_WRITABLE = (0x1000, 0xEA000)
UF2CONV = os.path.join(os.environ.get("ZEPHYR_BASE", ""), "scripts", "build", "uf2conv.py")

SERIAL_PREFIX = "BLEGW-"
# Matter core spec 5.1.7.1: setup codes 1..99999998, minus these
INVALID_PASSCODES = {0, 11111111, 22222222, 33333333, 44444444, 55555555, 66666666, 77777777, 88888888,
                     99999999, 12345678, 87654321}


def find_drive():
    drives = [os.path.dirname(p) for p in glob.glob("/Volumes/*/INFO_UF2.TXT")]
    if len(drives) != 1:
        sys.exit(
            "make-uf2: found %d UF2 drives%s. Double-tap RST to GND on the Pro Micro so it shows up as a drive."
            % (len(drives), (": " + ", ".join(drives)) if drives else "")
        )
    return drives[0]


def drive_chip_id(drive):
    """USB serial number of the device behind a mounted UF2 drive: the nRF52840's FICR DEVICEID in hex."""
    info = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", drive]))
    disk = info.get("ParentWholeDisk") or info["DeviceIdentifier"]
    roots = plistlib.loads(subprocess.check_output(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l", "-a"]))

    def find(node, usb):
        if "USB Serial Number" in node:
            usb = node  # innermost USB device on the way down, not a hub above it
        if node.get("BSD Name") == disk:
            return usb
        for child in node.get("IORegistryEntryChildren", []):
            found = find(child, usb)
            if found is not None:
                return found
        return None

    for root in roots:
        dev = find(root, None)
        if dev is not None:
            serial = dev["USB Serial Number"].upper()
            if not re.fullmatch(r"[0-9A-F]{16}", serial):
                sys.exit("make-uf2: %s reports USB serial %r, not a 64-bit chip ID; pass --chip-id" % (drive, serial))
            return serial
    sys.exit("make-uf2: no USB device found for %s; pass --chip-id" % drive)


def derive_identity(chip_id):
    """Matter identity of one board, derived from its chip ID. Changing this changes every board's setup code."""

    def h(label):
        return hashlib.sha256(("blegw-identity-v1:%s:%s" % (label, chip_id)).encode()).digest()

    passcode, n = 0, 0
    while passcode in INVALID_PASSCODES:
        passcode = 1 + int.from_bytes(h("passcode%d" % n)[:4], "big") % 99999998
        n += 1
    return {
        "sn": SERIAL_PREFIX + chip_id,
        "discriminator": int.from_bytes(h("discriminator")[:2], "big") & 0xFFF,
        "passcode": passcode,
        "spake2_salt": base64.b64encode(h("salt")).decode(),
        "rd_uid": h("rd_uid")[:16].hex(),
    }


def generator_command(build_dir):
    """The build's own factory data generator command (from build.ninja), as an argument list and its cwd."""
    ninja = os.path.join(build_dir, "build.ninja")
    with open(ninja) as f:
        for line in f:
            if "generate_nrfconnect_chip_factory_data.py" in line and line.lstrip().startswith("COMMAND"):
                m = re.match(r"\s*COMMAND = cd (\S+) && (.*)$", line)
                if m:
                    return shlex.split(m.group(2)), m.group(1)
    sys.exit("make-uf2: no factory data generator command in %s; build first" % ninja)


def set_arg(cmd, name, value):
    """Replaces (or adds) `name value` in an argument list."""
    if name in cmd:
        cmd[cmd.index(name) + 1] = value
    else:
        cmd += [name, value]


def generate_factory_data(build_dir, chip_id):
    """Generates factory data for one board. Returns (hex path, onboarding text)."""
    ident = derive_identity(chip_id)
    cmd, cwd = generator_command(build_dir)
    out_dir = os.path.join(os.path.abspath(build_dir), "identity", chip_id)
    os.makedirs(out_dir, exist_ok=True)

    set_arg(cmd, "--sn", ident["sn"])
    set_arg(cmd, "--discriminator", "0x%03X" % ident["discriminator"])
    set_arg(cmd, "--passcode", str(ident["passcode"]))
    set_arg(cmd, "--spake2_salt", ident["spake2_salt"])
    set_arg(cmd, "--rd_uid", ident["rd_uid"])
    set_arg(cmd, "-o", os.path.join(out_dir, "factory_data"))
    if "--spake2_verifier" in cmd:  # a fixed verifier would not match the new passcode and salt
        i = cmd.index("--spake2_verifier")
        del cmd[i:i + 2]

    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit("make-uf2: factory data generation failed:\n%s%s" % (res.stdout, res.stderr))

    onboarding = ""
    txt = os.path.join(out_dir, "factory_data.txt")
    if os.path.exists(txt):
        with open(txt) as f:
            onboarding = f.read().strip()
    summary = ("Chip ID %s\nSerial number %s, discriminator 0x%03X\n%s\nFiles: %s"
               % (chip_id, ident["sn"], ident["discriminator"], onboarding, out_dir))
    return os.path.join(out_dir, "factory_data.hex"), summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hex", nargs="*", help="hex files to merge (must not overlap)")
    ap.add_argument("-o", "--out", help="output .uf2")
    ap.add_argument("--deploy", action="store_true", help="copy the .uf2 to the UF2 bootloader drive")
    ap.add_argument("--identity", metavar="BUILD_DIR",
                    help="generate per-board factory data with the generator command of this sysbuild folder")
    ap.add_argument("--chip-id", help="chip ID (16 hex digits) instead of reading it from the UF2 drive")
    ap.add_argument("--show", action="store_true", help="with --identity: only print the board's codes")
    args = ap.parse_args()

    drive = find_drive() if args.deploy or (args.identity and not args.chip_id) else None

    hex_files = list(args.hex)
    if args.identity:
        chip_id = args.chip_id.upper() if args.chip_id else drive_chip_id(drive)
        if not re.fullmatch(r"[0-9A-F]{16}", chip_id):
            sys.exit("make-uf2: chip ID must be 16 hex digits")
        factory_hex, summary = generate_factory_data(args.identity, chip_id)
        print("make-uf2: identity of this board:\n  " + summary.replace("\n", "\n  "))
        if args.show:
            return
        hex_files.append(factory_hex)

    if not args.out or not hex_files:
        ap.error("need -o and at least one hex file")
    if not os.path.isfile(UF2CONV):
        sys.exit("make-uf2: uf2conv.py not found; source scripts/ncs-env.sh first")

    merged = IntelHex()
    for path in hex_files:
        merged.merge(IntelHex(path), overlap="error")
    for start, end in merged.segments():
        print("  0x%06x-0x%06x  %6d B" % (start, end, end - start))
        if start < UF2_WRITABLE[0] or end > UF2_WRITABLE[1]:
            sys.exit("make-uf2: 0x%06x-0x%06x is outside the range the UF2 bootloader writes (0x%06x-0x%06x)"
                     % (start, end, *UF2_WRITABLE))

    with tempfile.TemporaryDirectory() as tmp:
        tmp_hex = os.path.join(tmp, "merged.hex")
        merged.write_hex_file(tmp_hex)
        subprocess.run([sys.executable, UF2CONV, "-c", "-f", FAMILY_NRF52840, "-o", args.out, tmp_hex],
                       check=True, stdout=subprocess.DEVNULL)
    print("make-uf2: wrote %s" % args.out)

    if args.deploy:
        print("make-uf2: copying to %s" % drive)
        # -X: no extended attributes; macOS otherwise fails on some UF2 drives.
        # The board resets as soon as it has the file, so a late I/O error is expected.
        subprocess.run(["cp", "-X", args.out, drive])
        print("make-uf2: done; the board restarts into the gateway")


if __name__ == "__main__":
    main()
