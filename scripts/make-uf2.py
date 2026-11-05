#!/usr/bin/env python3
"""Build one .uf2 from the gateway's hex files and optionally copy it to a UF2 bootloader drive.

For the Pro Micro nRF52840 with the factory UF2 bootloader (make ... BOARD=promicro-uf2).
The app and the Matter factory data are separate hex files from sysbuild; both go
into one .uf2. The bootloader only writes 0x1000-0xea000, so everything must lie
in that range (see firmware/gateway/boards/promicro_nrf52840_nrf52840_uf2.overlay).

Usage:
  make-uf2.py -o gateway.uf2 [--deploy] app.hex factory_data.hex
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

from intelhex import IntelHex

FAMILY_NRF52840 = "0xADA52840"
# Adafruit_nRF52_Bootloader: USER_FLASH_START = MBR size, USER_FLASH_END = bootloader - 40 KB
UF2_WRITABLE = (0x1000, 0xEA000)
UF2CONV = os.path.join(os.environ.get("ZEPHYR_BASE", ""), "scripts", "build", "uf2conv.py")


def find_drive():
    drives = [os.path.dirname(p) for p in glob.glob("/Volumes/*/INFO_UF2.TXT")]
    if len(drives) != 1:
        sys.exit(
            "make-uf2: found %d UF2 drives%s. Double-tap RST to GND on the Pro Micro so it shows up as a drive."
            % (len(drives), (": " + ", ".join(drives)) if drives else "")
        )
    return drives[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hex", nargs="+", help="hex files to merge (must not overlap)")
    ap.add_argument("-o", "--out", required=True, help="output .uf2")
    ap.add_argument("--deploy", action="store_true", help="copy the .uf2 to the UF2 bootloader drive")
    args = ap.parse_args()

    if not os.path.isfile(UF2CONV):
        sys.exit("make-uf2: uf2conv.py not found; source scripts/ncs-env.sh first")

    merged = IntelHex()
    for path in args.hex:
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
        drive = find_drive()
        print("make-uf2: copying to %s" % drive)
        # -X: no extended attributes; macOS otherwise fails on some UF2 drives.
        # The board resets as soon as it has the file, so a late I/O error is expected.
        subprocess.run(["cp", "-X", args.out, drive])
        print("make-uf2: done; the board restarts into the gateway")


if __name__ == "__main__":
    main()
