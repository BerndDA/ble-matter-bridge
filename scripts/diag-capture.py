#!/usr/bin/env python3
"""Periodically run diagnostic shell commands on the gateway and log the output.

Needs the diagnostics build (firmware/ble-gateway/diag.conf) and a free serial port
(close nRF Terminal first). Run with the toolchain's Python, which has pyserial:

    source scripts/ncs-env.sh
    python3 scripts/diag-capture.py --minutes 60 --interval 60

Output: measurements/diag-<timestamp>.log (one block per sample, with timestamps).

With --thermostat, every --target-every samples an eQ-3 target is sent, cycling
through --targets (the last one is the target the thermostat is left at), e.g.
    python3 scripts/diag-capture.py --minutes 20 --thermostat 00:1A:22:0F:3F:02 --targets 18.5,18
"""

import argparse
import datetime
import glob
import os
import re
import sys
import time

import serial

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")

# Run every sample.
SAMPLE_COMMANDS = [
    "ot state",
    "ot rloc16",
    "ot counters mac",
    "ot counters mle",
    "matter stat peak",
    "kernel heap",
    "gateway seen",
    "gateway sensors",
]

# Run at start and end only (long output).
EDGE_COMMANDS = [
    "kernel uptime",
    "ot parent",
    "kernel thread stacks",
    "kernel thread list",
]


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    # The J-Link VCOM of the DK; other USB CDC devices (monitors etc.) may also be present.
    jlink = [p for p in ports if "000683" in p]
    return (jlink or ports or [None])[0]


def run(ser, cmd, wait=1.5):
    ser.reset_input_buffer()
    ser.write(cmd.encode() + b"\r\n")
    time.sleep(wait)
    out = ser.read(64 * 1024)
    return ANSI.sub(b"", out).decode(errors="replace").replace("\r", "")


# Log lines of the eQ-3 client, kept while waiting between samples.
EQ3_LINE = re.compile(r"Command 0x|Reply:|attempt|Attempt|Connect failed|failed after|Disconnected: |Subscribe|Discovery")


def wait_and_keep(ser, log, seconds):
    """Waits, but reads the serial port meanwhile and keeps the eQ-3 client's log lines."""
    end = time.time() + seconds
    rest = ""
    while time.time() < end:
        chunk = ANSI.sub(b"", ser.read(4096)).decode(errors="replace").replace("\r", "")
        lines = (rest + chunk).split("\n")
        rest = lines.pop()
        for line in lines:
            if EQ3_LINE.search(line):
                log.write(f"{datetime.datetime.now():%H:%M:%S} {line}\n")
    log.flush()


def sample(ser, log, commands, label):
    stamp = datetime.datetime.now().isoformat(timespec="seconds")
    log.write(f"\n===== {label} {stamp} =====\n")
    for cmd in commands:
        log.write(f"\n--- {cmd}\n")
        log.write(run(ser, cmd, wait=3 if "thread" in cmd else 1.5))
    log.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="serial port (default: auto-detect the DK)")
    ap.add_argument("--minutes", type=float, default=60, help="total capture time")
    ap.add_argument("--interval", type=float, default=60, help="seconds between samples")
    ap.add_argument("--out", default=None, help="log file (default: measurements/diag-<time>.log)")
    ap.add_argument("--ab", action="store_true",
                    help="A/B test: pause the BLE background scan for the second half of the capture")
    ap.add_argument("--thermostat", metavar="ADDR", help="eQ-3 thermostat to send targets to")
    ap.add_argument("--targets", default="18.5,18", help="targets to cycle through (default 18.5,18)")
    ap.add_argument("--target-every", type=int, default=2, help="send a target every N samples (default 2)")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No serial port found")

    out = args.out or datetime.datetime.now().strftime("measurements/diag-%Y%m%d-%H%M%S.log")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    ser = serial.Serial(port, 115200, timeout=0.3)
    end = time.time() + args.minutes * 60
    n = 0
    with open(out, "w") as log:
        log.write(f"# port {port}, {args.minutes} min, every {args.interval} s\n")
        sample(ser, log, EDGE_COMMANDS + SAMPLE_COMMANDS, "start")
        half = time.time() + args.minutes * 30
        paused = False
        print(f"Logging to {out} until {datetime.datetime.fromtimestamp(end):%H:%M}")
        while time.time() < end:
            wait_and_keep(ser, log, max(0.0, args.interval - 12))
            n += 1
            if args.ab and not paused and time.time() >= half:
                sample(ser, log, EDGE_COMMANDS, "before pause")
                log.write("--- gateway pause\n" + run(ser, f"gateway pause {int(end - time.time()) + 120}"))
                paused = True
                print("BLE scan paused (B phase)", flush=True)
            sample(ser, log, SAMPLE_COMMANDS, f"sample {n}{' (scan paused)' if paused else ''}")
            print(f"sample {n}", flush=True)
            if args.thermostat and n % args.target_every == 0:
                targets = args.targets.split(",")
                target = targets[(n // args.target_every - 1) % len(targets)]
                # The result arrives later as log lines ("Reply:" or "failed after"), inside later outputs.
                log.write(f"\n--- gateway target {args.thermostat} {target}\n"
                          + run(ser, f"gateway target {args.thermostat} {target}", wait=0.5))
                print(f"target {target}", flush=True)
        sample(ser, log, EDGE_COMMANDS + SAMPLE_COMMANDS, "end")
        if paused:
            log.write("--- gateway pause 0\n" + run(ser, "gateway pause 0"))
    print(f"Done: {out}")


if __name__ == "__main__":
    main()
