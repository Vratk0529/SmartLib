#!/usr/bin/env python3
"""List the SmartLib devices on the network.

Every SmartLib device answers "SMARTLIB?" on UDP port 3234 with a JSON
description of itself (name, hostname, IP, MAC, chip, firmware, uptime,
RSSI, MQTT/OTA/NTP state). This asks every address in the given ranges,
plus their broadcast addresses, and shows who answered.

    smartscan.py                       # all local IPv4 subnets, interactive
    smartscan.py 192.168.1.0/24        # CIDR
    smartscan.py 192.168.1.20-80       # range (last octet)
    smartscan.py 10.0.0.5-10.0.1.40    # range (full addresses)
    smartscan.py --once                # print a table and exit
    smartscan.py --json                # print JSON and exit

Keys: up/down or j/k select, Enter details, i identify (blink the LED),
r rescan, s change sort, q quit.

Linux only (uses `ip` to find the local subnets). No dependencies.
"""

import argparse
import curses
import ipaddress
import json
import socket
import subprocess
import sys
import threading
import time

PORT = 3234
PROBE = b"SMARTLIB?"
MAX_HOSTS = 4096  # refuse to unicast-scan anything bigger than a /20


# ---- address ranges -------------------------------------------------------------------------


def local_networks():
    """IPv4 networks of the up, non-loopback interfaces, from `ip -j addr`."""
    try:
        out = subprocess.run(["ip", "-j", "-4", "addr", "show", "up"], capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"cannot list interfaces ({e}); give a range, e.g. 192.168.1.0/24")

    nets = []
    for iface in json.loads(out):
        if "LOOPBACK" in iface.get("flags", []):
            continue
        for a in iface.get("addr_info", []):
            if a.get("family") == "inet":
                net = ipaddress.ip_network(f"{a['local']}/{a['prefixlen']}", strict=False)
                if net.prefixlen >= 20:  # skip huge ones (VPNs, docker /16s)
                    nets.append(net)
    if not nets:
        sys.exit("no local IPv4 subnet small enough to scan; give a range")
    return nets


def parse_range(text):
    """CIDR, a.b.c.d-e, a.b.c.d-w.x.y.z or a single address -> (hosts, broadcast or None)."""
    if "/" in text:
        net = ipaddress.ip_network(text, strict=False)
        return list(net.hosts()) or [net.network_address], net.broadcast_address

    if "-" in text:
        first, last = text.split("-", 1)
        start = ipaddress.ip_address(first)
        if "." in last:
            end = ipaddress.ip_address(last)
        else:
            end = ipaddress.ip_address(first.rsplit(".", 1)[0] + "." + last)
        if end < start:
            start, end = end, start
        count = int(end) - int(start) + 1
        if count > MAX_HOSTS:
            raise ValueError(f"{text}: {count} addresses, the limit is {MAX_HOSTS}")
        return [ipaddress.ip_address(int(start) + i) for i in range(count)], None

    return [ipaddress.ip_address(text)], None


def build_targets(ranges):
    hosts, broadcasts = [], set()
    specs = ranges or [str(n) for n in local_networks()]
    for spec in specs:
        try:
            h, bc = parse_range(spec)
        except ValueError as e:
            sys.exit(f"bad range {spec!r}: {e}")
        if len(h) > MAX_HOSTS:
            sys.exit(f"{spec}: {len(h)} addresses, the limit is {MAX_HOSTS}")
        hosts += h
        if bc is not None:
            broadcasts.add(bc)
    # Keep order, drop duplicates.
    seen = set()
    hosts = [h for h in hosts if not (h in seen or seen.add(h))]
    return specs, [str(h) for h in hosts], [str(b) for b in broadcasts]


# ---- scanning ---------------------------------------------------------------------------------


def scan(hosts, broadcasts, port=PORT, timeout=1.5, rounds=2):
    """Probe every host (and broadcast), return {key: info} of whoever answered."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", 0))
    sock.setblocking(False)

    found = {}

    def drain():
        while True:
            try:
                data, (addr, _) = sock.recvfrom(1024)
            except (BlockingIOError, InterruptedError):
                return
            try:
                info = json.loads(data.decode("utf-8", "replace"))
            except ValueError:
                continue
            if not isinstance(info, dict):
                continue
            info["_addr"] = addr
            info["_seen"] = time.time()
            found[info.get("mac") or addr] = info

    # Two rounds: a sleeping WiFi station or a full ARP queue can eat the first.
    for _ in range(rounds):
        for target in broadcasts + hosts:
            try:
                sock.sendto(PROBE, (target, port))
            except OSError:
                pass  # unreachable / no route: nobody there
            drain()
            time.sleep(0.0005)
        time.sleep(0.2)

    deadline = time.time() + timeout
    while time.time() < deadline:
        drain()
        time.sleep(0.02)
    sock.close()
    return found


def identify(addr, seconds=10, port=PORT):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    try:
        for _ in range(3):
            sock.sendto(b"SMARTLIB IDENTIFY %d" % seconds, (addr, port))
            try:
                data, _ = sock.recvfrom(64)
                if data.startswith(b"OK"):
                    return True
            except socket.timeout:
                pass
        return False
    finally:
        sock.close()


# ---- presentation ------------------------------------------------------------------------------


def fmt_uptime(s):
    if s is None:
        return ""
    s = int(s)
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if d:
        return f"{d}d{h:02}h"
    if h:
        return f"{h}h{m:02}m"
    return f"{m}m{s:02}s"


def fmt_bool(v):
    return {True: "yes", False: "no"}.get(v, "")


# (title, width, getter). Columns are dropped from the right when the terminal is narrow.
COLUMNS = [
    ("NAME", 20, lambda d: d.get("name", "")),
    ("IP", 15, lambda d: d.get("ip") or d.get("_addr", "")),
    ("HOST", 18, lambda d: d.get("host", "")),
    ("LINK", 8, lambda d: d.get("link", "")),
    ("RSSI", 5, lambda d: str(d["rssi"]) if d.get("link") == "wifi" and "rssi" in d else ""),
    ("UPTIME", 7, lambda d: fmt_uptime(d.get("uptime"))),
    ("MQTT", 4, lambda d: fmt_bool(d.get("mqtt"))),
    ("OTA", 4, lambda d: fmt_bool(d.get("ota"))),
    ("FW", 10, lambda d: d.get("fw", "")),
    ("CHIP", 14, lambda d: d.get("chip", "")),
    ("MAC", 17, lambda d: d.get("mac", "")),
    ("NTP", 15, lambda d: d.get("ntp", "")),
    ("RESET", 12, lambda d: d.get("reset", "")),
]

SORTS = [
    ("name", lambda d: (d.get("name", "").lower(), d.get("ip", ""))),
    ("ip", lambda d: ipaddress.ip_address(d.get("ip") or d.get("_addr") or "0.0.0.0")),
    ("uptime", lambda d: d.get("uptime") or 0),
    ("rssi", lambda d: -(d.get("rssi") or -999) if d.get("link") == "wifi" else 999),
]


def table_lines(devices, width=None):
    cols = COLUMNS
    if width is not None:
        cols, used = [], 0
        for c in COLUMNS:
            if used + c[1] + 1 > width and cols:
                break
            cols.append(c)
            used += c[1] + 1
    header = " ".join(t.ljust(w)[:w] for t, w, _ in cols)
    rows = [" ".join(str(g(d)).ljust(w)[:w] for _, w, g in cols) for d in devices]
    return header, rows


def print_once(devices):
    header, rows = table_lines(devices)
    print(header.rstrip())
    for r in rows:
        print(r.rstrip())
    print(f"\n{len(devices)} device(s)")


# ---- TUI ----------------------------------------------------------------------------------------


class App:
    def __init__(self, specs, hosts, broadcasts, args):
        self.specs, self.hosts, self.broadcasts, self.args = specs, hosts, broadcasts, args
        self.devices = {}  # key -> info, kept after a device stops answering
        self.lock = threading.Lock()
        self.scanning = False
        self.last_scan = 0.0
        self.selected = 0
        self.sort = 0
        self.detail = False
        self.status = ""

    def start_scan(self):
        if self.scanning:
            return
        self.scanning = True

        def run():
            found = scan(self.hosts, self.broadcasts, self.args.port, self.args.timeout)
            with self.lock:
                self.devices.update(found)
                self.last_scan = time.time()
                self.scanning = False

        threading.Thread(target=run, daemon=True).start()

    def sorted_devices(self):
        with self.lock:
            devs = list(self.devices.values())
        return sorted(devs, key=SORTS[self.sort][1])

    def draw(self, scr):
        scr.erase()
        h, w = scr.getmaxyx()
        devs = self.sorted_devices()
        self.selected = max(0, min(self.selected, len(devs) - 1))

        state = "scanning..." if self.scanning else (
            f"scanned {int(time.time() - self.last_scan)}s ago" if self.last_scan else "")
        title = f" SmartLib devices: {len(devs)}  |  {', '.join(self.specs)}  |  {state}"
        scr.addnstr(0, 0, title.ljust(w), w - 1, curses.A_REVERSE)

        if self.detail and devs:
            self.draw_detail(scr, devs[self.selected], h, w)
        else:
            header, rows = table_lines(devs, w - 1)
            scr.addnstr(1, 0, header, w - 1, curses.A_BOLD)
            stale_after = self.args.interval * 2 + 5
            for i, (row, d) in enumerate(zip(rows, devs)):
                y = 2 + i
                if y >= h - 1:
                    break
                attr = curses.A_REVERSE if i == self.selected else 0
                if time.time() - d["_seen"] > stale_after:
                    attr |= curses.A_DIM  # stopped answering
                scr.addnstr(y, 0, row.ljust(w - 1), w - 1, attr)
            if not devs and not self.scanning:
                scr.addnstr(3, 2, "Nobody answered. Wrong range, or firmware without discovery?", w - 3)

        help_ = " j/k select  Enter details  i identify  r rescan  s sort:" + SORTS[self.sort][0] + "  q quit"
        if self.status:
            help_ += "  |  " + self.status
        scr.addnstr(h - 1, 0, help_.ljust(w - 1), w - 1, curses.A_REVERSE)
        scr.refresh()

    def draw_detail(self, scr, d, h, w):
        # mDNS only exists on WiFi; Ethernet devices are flashed by IP.
        target = f"{d['host']}.local" if d.get("link") == "wifi" and d.get("host") else d.get("ip", d["_addr"])
        seen = int(time.time() - d["_seen"])
        lines = [(f"OTA: upload_protocol = espota, upload_port = {target}, upload_flags = --auth=<password>",
                  curses.A_DIM), ("", 0), (f"{'answered':>8}  {seen}s ago from {d['_addr']}", 0)]
        lines += [(f"{k:>8}  {d[k]}", 0) for k in sorted(k for k in d if not k.startswith("_"))]
        for y, (text, attr) in enumerate(lines, start=2):
            if y >= h - 1:
                break
            scr.addnstr(y, 2, text, w - 3, attr)

    def run(self, scr):
        curses.curs_set(0)
        curses.use_default_colors()
        scr.timeout(250)
        self.start_scan()

        while True:
            if not self.scanning and time.time() - self.last_scan > self.args.interval:
                self.start_scan()
            self.draw(scr)

            key = scr.getch()
            if key == -1:
                continue
            devs = self.sorted_devices()
            if key in (ord("q"), 27):
                if self.detail and key == 27:
                    self.detail = False
                else:
                    return
            elif key in (curses.KEY_DOWN, ord("j")):
                self.selected += 1
            elif key in (curses.KEY_UP, ord("k")):
                self.selected -= 1
            elif key in (curses.KEY_ENTER, 10, 13):
                self.detail = not self.detail
            elif key == ord("r"):
                self.start_scan()
            elif key == ord("s"):
                self.sort = (self.sort + 1) % len(SORTS)
            elif key == ord("i") and devs:
                d = devs[self.selected]
                addr = d.get("ip") or d["_addr"]
                ok = identify(addr, self.args.identify)
                self.status = (f"{d.get('name', addr)} blinking for {self.args.identify}s" if ok
                               else f"{d.get('name', addr)} did not answer")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="\n".join(__doc__.split("\n")[5:]))
    p.add_argument("ranges", nargs="*", help="CIDR, a.b.c.d-e, a.b.c.d-w.x.y.z or an address "
                                             "(default: local subnets)")
    p.add_argument("--once", action="store_true", help="print a table and exit")
    p.add_argument("--json", action="store_true", help="print JSON and exit")
    p.add_argument("--port", type=int, default=PORT)
    p.add_argument("--timeout", type=float, default=1.5, help="seconds to wait for answers")
    p.add_argument("--interval", type=float, default=15, help="rescan period in the TUI (s)")
    p.add_argument("--identify", type=int, default=10, help="LED blink time for 'i' (s)")
    args = p.parse_args()

    specs, hosts, broadcasts = build_targets(args.ranges)

    if args.once or args.json or not sys.stdout.isatty():
        found = scan(hosts, broadcasts, args.port, args.timeout)
        devices = sorted(found.values(), key=SORTS[0][1])
        if args.json:
            print(json.dumps([{k: v for k, v in d.items() if not k.startswith("_")} for d in devices],
                             indent=2))
        else:
            print_once(devices)
        return

    try:
        curses.wrapper(App(specs, hosts, broadcasts, args).run)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
