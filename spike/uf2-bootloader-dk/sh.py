#!/usr/bin/env python3
"""Send shell commands to the gateway over USB CDC: sh.py PORT 'cmd1' 'cmd2' ..."""
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
s.dtr = False; time.sleep(0.3); s.dtr = True; time.sleep(2); s.read(10000)
for c in sys.argv[2:]:
    s.write(c.encode() + b'\n'); time.sleep(1.5)
    print(s.read(8000).decode(errors='replace'))
