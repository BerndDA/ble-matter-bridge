#!/usr/bin/env python3
"""List the Matter nodes on the network with hostname and IPv6 addresses (make hosts).

Browses the operational Matter records (_matter._tcp) with dns-sd (macOS), resolves
each instance to its hostname and the hostname to its addresses. A node is listed once
however many fabrics (controllers: Apple Home, Home Assistant, ...) it is in. For our
gateways (and the Nuki bridge), the status page (http://<host>/api/status) adds which
one it is, board, firmware version and Thread role.

Address kinds: global (public, routed from the internet side of the border router
if the firewall lets it), ULA (fd00::/8, home network only). Link-local ones are left out.

Usage:
  matter-hosts.py              all nodes
  matter-hosts.py --no-web     skip the status page probe
"""
import argparse
import ipaddress
import json
import os
import pty
import re
import select
import subprocess
import sys
import threading
import time
import urllib.request

BROWSE_S = 3
RESOLVE_S = 3
ADDRESS_S = 3
QUIET_S = 0.5  # dns-sd never exits: stop this long after its last answer
WEB_TIMEOUT_S = 3


def dns_sd(args, max_s, answer):
    """Output lines of a dns-sd command: until QUIET_S after the last line matching
    @p answer (a regex), at most max_s. dns-sd runs on a pseudo-terminal, as it buffers
    its output when writing to a pipe."""
    master, slave = pty.openpty()
    proc = subprocess.Popen(["dns-sd", *args], stdout=slave, stderr=subprocess.DEVNULL)
    os.close(slave)
    text = ""
    end = time.monotonic() + max_s
    last_answer = None
    try:
        while True:
            now = time.monotonic()
            stop = end if last_answer is None else min(end, last_answer + QUIET_S)
            if now >= stop:
                break
            ready, _, _ = select.select([master], [], [], stop - now)
            if not ready:
                continue
            try:
                chunk = os.read(master, 4096)
            except OSError:
                break
            if not chunk:
                break
            new = chunk.decode(errors="replace")
            text += new
            if re.search(answer, new):
                last_answer = time.monotonic()
    finally:
        proc.kill()
        proc.wait()
        os.close(master)
    return text.replace("\r", "").splitlines()


def instances():
    found = set()
    for line in dns_sd(["-B", "_matter._tcp", "local."], BROWSE_S, r"\sAdd\s"):
        m = re.search(r"\sAdd\s.*_matter\._tcp\.\s+(\S+)\s*$", line)
        if m:
            found.add(m.group(1))
    return found


def resolve(instance):
    """Hostname (without the trailing dot) and port of a _matter._tcp instance."""
    out = "".join(dns_sd(["-L", instance, "_matter._tcp", "local."], RESOLVE_S, "can be reached at"))
    m = re.search(r"can be reached at (\S+?)\.?:(\d+)", out)
    return (m.group(1), int(m.group(2))) if m else (None, None)


def addresses(host):
    result = set()
    for line in dns_sd(["-G", "v6", host], ADDRESS_S, r"\sAdd\s"):
        m = re.search(r"\sAdd\s.*\s([0-9A-Fa-f:]*:[0-9A-Fa-f:]+)(%\S*)?\s+\d+\s*$", line)
        if not m:
            continue
        addr = ipaddress.IPv6Address(m.group(1))
        if not addr.is_link_local:
            result.add(addr)
    # global first, then ULA
    return sorted(result, key=lambda a: (not is_global(a), a))


def is_global(addr):
    return addr in ipaddress.IPv6Network("2000::/3")


def kind(addr):
    if is_global(addr):
        return "global"
    if addr in ipaddress.IPv6Network("fc00::/7"):
        return "ULA"
    return "other"


def gateway_info(host):
    """'EnOcean gateway 0.1.0, promicro_nrf52840, router' from the status page, or ''."""
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=WEB_TIMEOUT_S) as r:
            status = json.load(r)
    except Exception:
        return ""
    system = status.get("system", {})
    if "tcm" in status:
        what = "EnOcean gateway"
    elif "sensors" in status:
        what = "BLE gateway"
    elif "lock" in status:
        what = "Nuki bridge"
    else:
        what = "status page"
    parts = [f"{what} {system.get('version', '?')}"]
    if system.get("board"):
        parts.append(system["board"])
    role = status.get("thread", {}).get("role")
    if role:
        parts.append(role)
    return ", ".join(parts)


def parallel(func, items, deadline_s=None):
    """{item: func(item)}, run in parallel; items not done within deadline_s are left out."""
    results = {}
    lock = threading.Lock()

    def run(item):
        value = func(item)
        with lock:
            results[item] = value

    threads = [threading.Thread(target=run, args=(i,), daemon=True) for i in items]
    for t in threads:
        t.start()
    end = time.monotonic() + deadline_s if deadline_s else None
    for t in threads:
        t.join(None if end is None else max(0, end - time.monotonic()))
    with lock:
        return dict(results)


def main():
    parser = argparse.ArgumentParser(description="List Matter nodes with hostname and IPv6 addresses.")
    parser.add_argument("--no-web", action="store_true", help="don't ask the status page")
    args = parser.parse_args()

    found = instances()
    if not found:
        print("No Matter nodes found (_matter._tcp).", file=sys.stderr)
        return 1

    # host -> number of fabrics (instances)
    fabrics = {}
    for host, _ in parallel(resolve, found).values():
        if host:
            fabrics[host] = fabrics.get(host, 0) + 1

    hosts = sorted(fabrics)
    addrs = parallel(addresses, hosts)
    info = {} if args.no_web else parallel(gateway_info, hosts, WEB_TIMEOUT_S)

    for host in hosts:
        line = f"{host}  ({fabrics[host]} fabric{'s' if fabrics[host] != 1 else ''})"
        if info.get(host):
            line += f"  {info[host]}"
        print(line)
        if info.get(host):
            print(f"  http://{host}/")
        for a in addrs[host]:
            print(f"  {kind(a):6}  {a}")
        if not addrs[host]:
            print("  (no address resolved)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
