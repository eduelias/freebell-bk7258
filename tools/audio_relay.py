#!/usr/bin/env python3
"""One-shot TCP audio relay for the doorbell's `mic rec` / `spk net` commands (firmware v0.12.0+).

The doorbell always connects OUT, so this runs on a LAN host it can reach (du7; the Mac's firewall
blocks incoming connections). Each invocation serves exactly one connection, then exits.

  audio_relay.py recv PORT FILE [TIMEOUT]   # accept one connection, write everything it sends to FILE
  audio_relay.py send PORT FILE [TIMEOUT]   # accept one connection, send FILE (s16le mono 8 kHz), close

On du7 run it detached:  systemd-run --user --unit=relay-recv python3 ~/audio_relay.py recv 5100 /tmp/mic.raw
Then on the doorbell:    mic rec 4000 192.168.2.68 5100      (raw 32-bit ADC FIFO words)
                         spk net 192.168.2.68 5101            (plays the file the relay serves)
"""
import socket, sys, time


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("recv", "send"):
        sys.exit(__doc__)
    mode, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    timeout = float(sys.argv[4]) if len(sys.argv) > 4 else 120.0
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", port))
    ls.listen(1)
    ls.settimeout(timeout)
    print(f"{mode}: listening on {port} for {timeout:.0f}s", flush=True)
    conn, peer = ls.accept()
    conn.settimeout(30)
    t0 = time.time()
    total = 0
    if mode == "recv":
        with open(path, "wb") as f:
            while True:
                try:
                    chunk = conn.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                f.write(chunk)
                total += len(chunk)
    else:
        with open(path, "rb") as f:
            while True:
                chunk = f.read(4096)
                if not chunk:
                    break
                conn.sendall(chunk)
                total += len(chunk)
    conn.close()
    print(f"{mode}: {total} bytes with {peer[0]} in {time.time() - t0:.1f}s", flush=True)


if __name__ == "__main__":
    main()
