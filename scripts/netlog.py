#!/usr/bin/env python3
"""Receive a gateway's log output over the network (make netlog).

Finds gateways through their Matter mDNS records (dns-sd, macOS), asks each
Matter node on UDP port 5140 whether it is a gateway, subscribes to the chosen
one and prints what it sends until Ctrl-C. The subscription is renewed every
5 s; the gateway stops sending a minute after the last renewal. Protocol:
firmware/common/netlog/net_log.h.

Usage:
  netlog.py                      the only gateway found (or a list to choose from)
  netlog.py --serial 9E749B      the gateway whose serial number contains this
  netlog.py --host <IPv6>        skip discovery
  netlog.py --debug              include debug lines (much more Thread traffic)
"""
import argparse
import re
import socket
import subprocess
import sys
import threading
import time

PORT = 5140
PREFIX = b"blegw-log "
RENEW_S = 5  # also how soon the log resumes after the gateway restarts


def run_for(cmd, seconds):
    """Runs a never-ending dns-sd command for a while and returns its output."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        out, _ = proc.communicate(timeout=seconds)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
    return out


def matter_hosts():
    """Host names of all Matter nodes on the network (_matter._tcp, operational records)."""
    instances = set()
    for line in run_for(["dns-sd", "-B", "_matter._tcp", "local."], 3).splitlines():
        m = re.search(r"\sAdd\s.*_matter\._tcp\.\s+(\S+)\s*$", line)
        if m:
            instances.add(m.group(1))

    hosts = set()
    lock = threading.Lock()

    def lookup(instance):
        out = run_for(["dns-sd", "-L", instance, "_matter._tcp", "local."], 3)
        m = re.search(r"can be reached at (\S+?)\.?:\d+", out)
        if m:
            with lock:
                hosts.add(m.group(1))

    threads = [threading.Thread(target=lookup, args=(i,)) for i in instances]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return hosts


def addresses(host):
    try:
        infos = socket.getaddrinfo(host, PORT, socket.AF_INET6, socket.SOCK_DGRAM)
    except socket.gaierror:
        return []
    return sorted({info[4][0] for info in infos if not info[4][0].startswith("fe80")})


def find_gateways(sock):
    """Sends hello to every Matter node; returns {address: reply} of those that answer."""
    print("netlog: looking for gateways (Matter mDNS records) ...", file=sys.stderr)
    targets = set()
    for host in matter_hosts():
        targets.update(addresses(host))
    for addr in targets:
        try:
            sock.sendto(PREFIX + b"hello", (addr, PORT))
        except OSError:
            pass

    found = {}
    deadline = time.time() + 2
    while time.time() < deadline:
        sock.settimeout(max(0.05, deadline - time.time()))
        try:
            data, src = sock.recvfrom(2048)
        except socket.timeout:
            break
        if data.startswith(PREFIX + b"gateway "):
            found[src[0]] = data[len(PREFIX) + len("gateway "):].decode(errors="replace")
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="gateway IPv6 address; skips discovery")
    ap.add_argument("--serial", help="pick the gateway whose serial number contains this")
    ap.add_argument("--debug", action="store_true", help="include debug lines")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.bind(("::", 0))

    if args.host:
        gateway = args.host
    else:
        found = find_gateways(sock)
        if args.serial:
            found = {a: r for a, r in found.items() if args.serial.upper() in r.upper()}
        if not found:
            sys.exit("netlog: no gateway answered. Is it running firmware with net log (debug build)? "
                     "Or pass --host <IPv6>.")
        if len(found) > 1:
            for addr, reply in found.items():
                print("  %s  %s" % (reply, addr), file=sys.stderr)
            sys.exit("netlog: several gateways; pick one with --serial")
        gateway, reply = next(iter(found.items()))
        print("netlog: %s at %s" % (reply, gateway), file=sys.stderr)

    command = PREFIX + (b"subscribe debug" if args.debug else b"subscribe")
    stop = threading.Event()

    def renew():
        while not stop.is_set():
            try:
                sock.sendto(command, (gateway, PORT))
            except OSError as e:
                print("netlog: %s" % e, file=sys.stderr)
            stop.wait(RENEW_S)

    threading.Thread(target=renew, daemon=True).start()
    sock.settimeout(None)
    out = sys.stdout.buffer
    try:
        while True:
            data, src = sock.recvfrom(4096)
            if src[0] != gateway:
                continue
            if data.startswith(PREFIX):
                if data == PREFIX + b"ok" and not getattr(main, "subscribed", False):
                    main.subscribed = True
                    print("netlog: subscribed (Ctrl-C ends)", file=sys.stderr)
                continue
            out.write(data)
            out.flush()
    except KeyboardInterrupt:
        stop.set()
        sock.sendto(PREFIX + b"stop", (gateway, PORT))
        print("\nnetlog: stopped", file=sys.stderr)


if __name__ == "__main__":
    main()
