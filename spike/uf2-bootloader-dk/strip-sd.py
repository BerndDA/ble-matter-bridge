#!/usr/bin/env python3
"""Turn an Adafruit *_s140_*.hex into the *_nosd.hex equivalent: MBR + bootloader, no SoftDevice."""
import sys
from intelhex import IntelHex
src, dst = sys.argv[1:3]
ih = IntelHex(src)
for a in range(0x1000, 0x26000):
    if a in ih._buf:
        del ih[a]
ih.write_hex_file(dst)
for s, e in ih.segments():
    print(hex(s), hex(e))
