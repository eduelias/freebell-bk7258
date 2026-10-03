"""Send a command to the doorbell over the LAN (firmware v0.9.0+), no serial cable needed.

  python3 tools/dbcmd.py hook ring
  python3 tools/dbcmd.py ota http://192.168.2.80:8123/local/doorbell/doorbell-fw_0.9.1_app_pack.rbl
  python3 tools/dbcmd.py reboot

Protocol: one TCP connection to port 2323, one line "<token> <command>\n", reply "ok" /
"unknown" / "denied". Command output itself goes to the device's UART, not back over TCP.
Token: env DOORBELL_TOKEN, else TOKEN= in ~/data/doorbell-bk7258/tcp_token.env (the value given
to `tok set` over serial). Host: env DOORBELL_HOST, default 192.168.2.89.
"""
import os, socket, sys


def token():
    t = os.environ.get("DOORBELL_TOKEN")
    if t:
        return t
    try:
        for line in open(os.path.expanduser("~/data/doorbell-bk7258/tcp_token.env")):
            if line.startswith("TOKEN="):
                return line.strip().split("=", 1)[1]
    except FileNotFoundError:
        pass
    sys.exit("no token: set DOORBELL_TOKEN or ~/data/doorbell-bk7258/tcp_token.env")


def send(cmd, host=None, timeout=8.0):
    host = host or os.environ.get("DOORBELL_HOST", "192.168.2.89")
    try:
        with socket.create_connection((host, 2323), timeout=timeout) as s:
            s.sendall(f"{token()} {cmd}\n".encode())
            return s.recv(64).decode(errors="replace").strip()
    except OSError as e:
        sys.exit(f"cannot reach {host}:2323 ({e.strerror or e}); is the device up and the token set?")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print(send(" ".join(sys.argv[1:])))
