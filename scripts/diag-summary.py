#!/usr/bin/env python3
"""Summarize a capture from diag-capture.py.

    python3 scripts/diag-summary.py measurements/diag-ab-<time>.log

Counters are cumulative since boot, so each phase is reported as the difference
between its first and last sample. With --ab captures, phase A has the BLE
background scan running and phase B has it paused.
"""

import re
import sys
from datetime import datetime

BLOCK = re.compile(r"^===== (.+?) (\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d) =====$", re.M)
# The capture writes "--- <command>" right after the shell prompt of the previous output.
CMD = re.compile(r"^(?:uart:~\$ )?--- (.+)$", re.M)
COUNTER = re.compile(r"^\s*([A-Za-z ]+?):\s+(-?\d+)\s*$", re.M)
SEEN_ROW = re.compile(
    r"^(\S+) \((public|random)\)\s+(-?\d+)\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(\d+)\s+(\d+)\s+(\d+)s", re.M
)
STACK_ROW = re.compile(r"^0x[0-9a-f]+ (.+?)\s+\(real size\s+(\d+)\):\s+unused\s+(\d+)\s+usage\s+(\d+)", re.M)


def parse(path):
    text = open(path, errors="replace").read()
    samples = []
    heads = list(BLOCK.finditer(text))
    for i, h in enumerate(heads):
        body = text[h.end(): heads[i + 1].start() if i + 1 < len(heads) else len(text)]
        cmds = {}
        parts = list(CMD.finditer(body))
        for j, c in enumerate(parts):
            cmds[c.group(1).strip()] = body[c.end(): parts[j + 1].start() if j + 1 < len(parts) else len(body)]
        samples.append({"label": h.group(1), "time": datetime.fromisoformat(h.group(2)), "cmds": cmds})
    return samples


def counters(sample, cmd):
    out = sample["cmds"].get(cmd, "")
    return {k.strip(): int(v) for k, v in COUNTER.findall(out)}


def seen(sample):
    rows = {}
    for addr, _, rssi, age, meas, pkts, gap in SEEN_ROW.findall(sample["cmds"].get("gateway seen", "")):
        rows[addr] = {"rssi": int(rssi), "meas": int(meas), "pkts": int(pkts), "max_gap": int(gap)}
    return rows


def StateOf(sample):
    """Thread role printed by `ot state` (the line after the echoed command)."""
    lines = [l.strip() for l in sample["cmds"].get("ot state", "").splitlines() if l.strip()]
    roles = [l for l in lines if l in ("disabled", "detached", "child", "router", "leader")]
    return roles[0] if roles else ""


def delta(a, b, key):
    return b.get(key, 0) - a.get(key, 0)


def phase_report(name, first, last):
    minutes = (last["time"] - first["time"]).total_seconds() / 60
    print(f"\n## Phase {name}: {first['time']:%H:%M} - {last['time']:%H:%M} ({minutes:.0f} min)")
    if minutes <= 0:
        print("  (not enough samples)")
        return

    m0, m1 = counters(first, "ot counters mac"), counters(last, "ot counters mac")
    tx = delta(m0, m1, "TxTotal")
    tx_ack_req = delta(m0, m1, "TxAckRequested")
    retry = delta(m0, m1, "TxRetry")
    print("Thread MAC:")
    print(f"  TxTotal {tx}, TxAckRequested {tx_ack_req}, TxRetry {retry}"
          f" ({100 * retry / tx_ack_req:.1f} % of acked frames)" if tx_ack_req else f"  TxTotal {tx}")
    for key in ("TxErrCca", "TxErrBusyChannel", "TxErrAbort", "TxDirectMaxRetryExpiry", "TxIndirectMaxRetryExpiry",
                "RxTotal", "RxErrFcs", "RxErrNoFrame", "RxErrOther"):
        print(f"  {key} {delta(m0, m1, key)}")
    print(f"  RxTotal per minute {delta(m0, m1, 'RxTotal') / minutes:.0f}")

    l0, l1 = counters(first, "ot counters mle"), counters(last, "ot counters mle")
    print("Thread MLE:")
    for key in ("Role Detached", "Role Child", "Role Router", "Role Leader", "Partition Id Changes", "Parent Changes",
                "Attach Attempts"):
        print(f"  {key} +{delta(l0, l1, key)}")
    states = sorted({StateOf(s) for s in (first, last)} - {""})
    print(f"  state at edges: {', '.join(states)}")

    s0, s1 = seen(first), seen(last)
    print("BLE sensors:")
    for addr, r1 in s1.items():
        r0 = s0.get(addr, {"pkts": 0, "meas": 0})
        pkts, meas = r1["pkts"] - r0["pkts"], r1["meas"] - r0["meas"]
        expected = minutes * 60 / 2.5  # pvvx advertising interval 2.5 s
        print(f"  {addr} RSSI {r1['rssi']}: {pkts / minutes:.1f} pkts/min"
              f" ({100 * pkts / expected:.0f} % of advertisements), {meas / minutes:.1f} meas/min,"
              f" max gap since boot {r1['max_gap']} s")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    samples = parse(sys.argv[1])
    if len(samples) < 2:
        sys.exit("Need at least two samples")

    pause_idx = next((i for i, s in enumerate(samples) if s["label"] == "before pause"), None)
    regular = [s for s in samples if s["label"] != "before pause"]

    print(f"# {sys.argv[1]}: {len(samples)} samples")
    if pause_idx is None:
        phase_report("all", regular[0], regular[-1])
    else:
        # "before pause" holds only the edge commands; A ends at the last full sample before it.
        phase_report("A (BLE scan running)", samples[0], samples[pause_idx - 1])
        b = samples[pause_idx + 1:]
        if len(b) >= 2:
            phase_report("B (BLE scan paused)", b[0], b[-1])

    text = open(sys.argv[1], errors="replace").read()
    sent = len(re.findall(r"^--- gateway target ", text, re.M))
    if sent:
        print("\n## eQ-3 thermostat commands")
        print(f"  sent {sent}, answered {text.count('I: Reply:')}, failed {text.count('failed after')}")
        print(f"  attempts {len(re.findall(r'I: Command 0x.. to .*attempt', text))},"
              f" connect failures {text.count('Connect failed')},"
              f" dropped at connect (HCI 0x3E) {len(re.findall(r'Disconnected: .*reason 62', text))},"
              f" attempt timeouts {text.count('Attempt timed out')}")

    last_with_stacks = [s for s in samples if "kernel thread stacks" in s["cmds"]][-1]
    print("\n## Stacks (peak use since boot)")
    for name, size, unused, used in STACK_ROW.findall(last_with_stacks["cmds"]["kernel thread stacks"]):
        flag = "  <-- above 70 %" if int(used) > 0.7 * int(size) else ""
        print(f"  {name.strip():20s} {used:>5}/{size:>5} B ({100 * int(used) // int(size)} %){flag}")

    print("\n## Matter heap and resources (last `matter stat peak`)")
    print(samples[-1]["cmds"].get("matter stat peak", "(missing)").strip())
    print("\n## System heap (last `kernel heap`)")
    print(samples[-1]["cmds"].get("kernel heap", "(missing)").strip())


if __name__ == "__main__":
    main()
