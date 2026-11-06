#!/usr/bin/env python3
"""Build one .uf2 from the gateway's hex files and optionally copy it to a UF2 bootloader drive.

For the Pro Micro nRF52840 with the factory UF2 bootloader (make ... BOARD=promicro-uf2).
The app and the Matter factory data go into one .uf2. The bootloader only writes
0x1000-0xea000, so everything must lie in that range (see
firmware/gateway/boards/promicro_nrf52840_nrf52840_uf2.overlay).

Per-board identity: the firmware derives serial number, discriminator, setup code,
SPAKE2+ salt and rotating device ID from the chip ID (FICR DEVICEID) at boot
(CONFIG_GATEWAY_IDENTITY_FROM_CHIP_ID, firmware/gateway/src/identity/). The UF2
bootloader reports the same chip ID as its USB serial number, so --identity
BUILD_DIR computes the same values here (derive_identity) and prints the board's
setup code. The generated factory data under BUILD_DIR/identity/<chip ID>/ is only
used for that printout and the QR code image; the .uf2 carries the build's own
factory data, the same for every board.

Usage:
  make-uf2.py -o gateway.uf2 [--deploy] [--identity BUILD_DIR] [--port PORT] app.hex factory_data.hex
              --port: without a UF2 drive, sends `gateway bootloader` on that port first
  make-uf2.py --identity BUILD_DIR [--chip-id 54932B32846202D7 | --jlink]   (codes of a board only;
              --jlink reads the chip ID over the J-Link, e.g. from the DK)
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
import threading
import time

from intelhex import IntelHex

FAMILY_NRF52840 = "0xADA52840"
# Adafruit_nRF52_Bootloader: USER_FLASH_START = MBR size, USER_FLASH_END = bootloader - 40 KB
UF2_WRITABLE = (0x1000, 0xEA000)
UF2CONV = os.path.join(os.environ.get("ZEPHYR_BASE", ""), "scripts", "build", "uf2conv.py")

ZEPHYR_USB_VID = 0x2FE3  # CONFIG_CDC_ACM_SERIAL_VID
SERIAL_PREFIX = "BLEGW-"
# Matter core spec 5.1.7.1: setup codes 1..99999998, minus these
INVALID_PASSCODES = {0, 11111111, 22222222, 33333333, 44444444, 55555555, 66666666, 77777777, 88888888,
                     99999999, 12345678, 87654321}


def jlink_chip_id():
    """FICR DEVICEID over the J-Link (nrfjprog), as the 16 hex digits the firmware and the bootloader use."""
    res = subprocess.run(["nrfjprog", "--memrd", "0x10000060", "--n", "8"], capture_output=True, text=True)
    m = re.search(r"0x10000060:\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})", res.stdout)
    if not m:
        sys.exit("make-uf2: cannot read the chip ID over the J-Link; pass --chip-id\n%s" % res.stderr.strip())
    return (m.group(2) + m.group(1)).upper()  # DEVICEID[1] DEVICEID[0]


def uf2_drives():
    return [os.path.dirname(p) for p in glob.glob("/Volumes/*/INFO_UF2.TXT")]


def enter_bootloader(port, timeout_s=20):
    """Sends `gateway bootloader` over the gateway's USB shell and waits for the UF2 drive."""
    import serial  # pyserial, part of the NCS toolchain
    from serial.tools import list_ports

    # Only talk to the gateway (Zephyr's CDC ACM vendor ID), not to some other usbmodem device.
    vids = {p.device: p.vid for p in list_ports.comports()}
    if vids.get(port) != ZEPHYR_USB_VID:
        sys.exit("make-uf2: no UF2 drive, and %s is not the gateway's USB shell. Double-tap RST to GND." % port)

    print("make-uf2: no UF2 drive; restarting the gateway on %s into the bootloader" % port)
    result = {}

    def send():
        try:
            with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as s:
                s.write(b"\ngateway bootloader\n")
                s.flush()
                time.sleep(0.5)
            result["ok"] = True
        except (serial.SerialException, OSError) as e:
            result["error"] = e

    # A hung gateway can block even the open() of its port; don't wait for it forever.
    sender = threading.Thread(target=send, daemon=True)
    sender.start()
    sender.join(5)
    if "ok" not in result:
        reason = result.get("error", "the gateway doesn't answer")
        sys.exit("make-uf2: cannot use %s (%s). Close `make term`, or double-tap RST to GND." % (port, reason))

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if uf2_drives():
            time.sleep(1)  # let macOS finish mounting
            return
        time.sleep(0.5)
    sys.exit("make-uf2: no UF2 drive after %d s. Is the firmware older than `gateway bootloader`? "
             "Double-tap RST to GND instead." % timeout_s)


def find_drive(port=None):
    drives = uf2_drives()
    if not drives and port and os.path.exists(port):
        enter_bootloader(port)
        drives = uf2_drives()
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
                    help="print the board's setup code, computed with the generator command of this sysbuild folder")
    ap.add_argument("--chip-id", help="chip ID (16 hex digits) instead of reading it from the UF2 drive")
    ap.add_argument("--jlink", action="store_true", help="read the chip ID over the J-Link instead")
    ap.add_argument("--port", help="gateway shell port: without a UF2 drive, restart the gateway into the bootloader")
    args = ap.parse_args()

    drive = find_drive(args.port) if args.deploy or (args.identity and not (args.chip_id or args.jlink)) else None


    hex_files = list(args.hex)
    if args.identity:
        if args.chip_id:
            chip_id = args.chip_id.upper()
        elif args.jlink:
            chip_id = jlink_chip_id()
        else:
            chip_id = drive_chip_id(drive)
        if not re.fullmatch(r"[0-9A-F]{16}", chip_id):
            sys.exit("make-uf2: chip ID must be 16 hex digits")
        _, summary = generate_factory_data(args.identity, chip_id)
        print("make-uf2: identity of this board (derived by the firmware at boot):\n  "
              + summary.replace("\n", "\n  "))
        if not args.out and not hex_files:
            return

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
