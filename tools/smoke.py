"""On-device smoke test: a read-only snapshot of the doorbell's behaviour, and a diff between two runs.

  python3 tools/smoke.py run > before.txt        # device awake (`pwr sleep off` while testing)
  python3 tools/smoke.py run > after.txt         # after the OTA
  python3 tools/smoke.py diff before.txt after.txt

`run` fetches `/snapshot.jpg` first (so the camera is open for every run), then sends only read-only
commands (status/show/stat, `pin dump N` for every pin config word), fetches the HTTP status page and `/cmd`, and does an RTSP OPTIONS/DESCRIBE handshake. Nothing rings, plays,
records, saves to the SD card or changes a setting. The camera opens for the snapshot, as it does
for Home Assistant. Secrets are masked (webhook id, token).

`diff` compares the two runs line by line. A line whose words are the same and only numbers moved
(uptime, millivolts, counters) counts as "numbers only" and is listed compactly; anything else
(a renamed setting, a different pin mode, a missing line) is a CHANGE and makes the exit status 1.
Host and token come from the same settings as tools/dbcmd.py (--host/--token, environment or
~/.config/doorbell-bk7258/env).
"""
import argparse
import http.client
import os
import re
import socket
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcmd  # noqa: E402

COMMANDS = [
    "sys info", "wifi status", "hook show", "pwr status", "pir status", "night status", "cam stat",
    "rtsp status", "talk status", "sd status", "vol", "log stat",
]
EXACT = ("pin cfg",)   # blocks compared exactly by `diff` (pin direction/pull/function must not move)
# AON GPIO config word (soc/bk7258/soc/gpio_struct.h): bit 0 = live input level, 1 output, 2 input_en,
# 3 output_en, 4 pull up/down, 5 pull_en, 6 second function, 7 input monitor, 8-9 drive, 10-11 int
# type, 12 int_en, 13 int_clear, 14-31 reserved. Compare bits 1-12 only: bit 0 and 13 are live state,
# and the reserved bits change between runs of the same firmware (seen on GPIO 11 and 27).
PIN_CFG_MASK = 0x1FFE
NUM = re.compile(r"[-+]?(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?)")


def mask(text):
    text = re.sub(r"(/api/webhook/)[\w-]+", r"\1<id>", text)
    return re.sub(r"(?<![\w])(t=)[^&\s]+", r"\1<token>", text)


def http_get(host, port, path, timeout=15.0):
    c = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        c.request("GET", path)
        r = c.getresponse()
        return r.status, r.getheader("Content-Type", ""), r.read()
    finally:
        c.close()


def rtsp(host, port=554, timeout=8.0):
    out = []
    with socket.create_connection((host, port), timeout=timeout) as s:
        for cseq, req in enumerate(("OPTIONS", "DESCRIBE"), 1):
            extra = "Accept: application/sdp\r\n" if req == "DESCRIBE" else ""
            s.sendall(f"{req} rtsp://{host}:{port}/live RTSP/1.0\r\nCSeq: {cseq}\r\n{extra}\r\n".encode())
            time.sleep(0.5)
            data = s.recv(8192).decode(errors="replace")
            keep = [ln for ln in data.splitlines()
                    if ln.startswith(("RTSP/", "Public:", "m=", "a=rtpmap", "a=control", "a=sendonly", "a=recvonly"))]
            out.append(f"{req}: " + " | ".join(keep))
    return out


def run(args):
    host = dbcmd.resolve_host(args.host)
    print(f"# smoke run {time.strftime('%Y-%m-%d %H:%M:%S')} host {host}")
    print("## http /snapshot.jpg")   # first: every run then sees the camera open
    st, ct, body = http_get(host, args.http_port, "/snapshot.jpg", timeout=20)
    print(st, ct, "jpeg" if body[:2] == b"\xff\xd8" else "NOT-JPEG", "size>10k" if len(body) > 10000 else f"size {len(body)}")
    time.sleep(2)
    for cmd in COMMANDS:
        reply = dbcmd.send(cmd, host=host, port=args.port, tok=args.token, timeout=10)
        print(f"## {cmd}")
        print(mask(reply))
        time.sleep(0.3)   # the firmware's TCP task drops commands sent back to back
    print("## pin cfg")   # AON config word per pin, configuration bits only (PIN_CFG_MASK)
    for first in range(0, 56, 8):
        reply = dbcmd.send(f"pin dump {first}", host=host, port=args.port, tok=args.token, timeout=10)
        m = re.search(r"CFG (\d+): ((?:[0-9a-f]{8} ?){8})", reply)
        words = " ".join("%04x" % (int(w, 16) & PIN_CFG_MASK) for w in m.group(2).split()) if m else "no reply: " + reply
        print(f"CFG {first:02d}: {words}")
        time.sleep(0.3)
    print("## http /")
    st, ct, body = http_get(host, args.http_port, "/")
    print(st, ct, body.decode(errors="replace").strip()[:300])
    print("## http /cmd sys info")
    tok = dbcmd.token(args.token)
    st, ct, body = http_get(host, args.http_port, "/cmd?t=%s&c=%s" % (urllib.parse.quote(tok), urllib.parse.quote("sys info")))
    print(st, ct, mask(body.decode(errors="replace").strip()))
    print("## rtsp")
    try:
        for ln in rtsp(host, args.rtsp_port):
            print(ln)
    except OSError as e:
        print("rtsp unreachable:", e)
    return 0


def blocks(path):
    out, cur = {}, None
    for ln in open(path, encoding="utf-8", errors="replace").read().splitlines():
        if ln.startswith("## "):
            cur = ln[3:]
            out[cur] = []
        elif cur is not None:
            out[cur].append(ln)
    return out


def diff(args):
    a, b = blocks(args.before), blocks(args.after)
    changes = 0
    for key in list(a) + [k for k in b if k not in a]:
        la, lb = a.get(key), b.get(key)
        if la is None or lb is None:
            print(f"CHANGE [{key}]: only in {'after' if la is None else 'before'}")
            changes += 1
            continue
        numeric = 0
        for i in range(max(len(la), len(lb))):
            x = la[i] if i < len(la) else "<missing>"
            y = lb[i] if i < len(lb) else "<missing>"
            if x == y:
                continue
            if key not in EXACT and NUM.sub("#", x) == NUM.sub("#", y):
                numeric += 1
                if args.verbose:
                    print(f"  numbers [{key}]: {x}\n                -> {y}")
                continue
            print(f"CHANGE [{key}]:\n  - {x}\n  + {y}")
            changes += 1
        if numeric and not args.verbose:
            print(f"  numbers only [{key}]: {numeric} line(s)")
    print(f"{changes} change(s)")
    return 1 if changes else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--host")
    p.add_argument("--token")
    p.add_argument("--port", type=int, help="TCP command port (default 2323 / DOORBELL_PORT)")
    p.add_argument("--http-port", type=int, default=8080)
    p.add_argument("--rtsp-port", type=int, default=554)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("run")
    d = sub.add_parser("diff")
    d.add_argument("before")
    d.add_argument("after")
    d.add_argument("-v", "--verbose", action="store_true", help="also print the numbers-only lines")
    args = p.parse_args()
    return run(args) if args.cmd == "run" else diff(args)


if __name__ == "__main__":
    sys.exit(main())
