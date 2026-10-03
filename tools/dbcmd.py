"""Send a command to the doorbell over the LAN (firmware v0.9.0+), no serial cable needed.

  python3 tools/dbcmd.py hook ring
  python3 tools/dbcmd.py ota http://192.168.2.80:8123/local/doorbell/doorbell-fw_0.9.1_app_pack.rbl
  python3 tools/dbcmd.py reboot

Protocol: one TCP connection to port 2323, one line "<token> <command>\n", reply "ok" /
"unknown" / "denied", then the command's REPLY() text (v0.19+); the rest of the output goes to
the device's UART. Token: --token, else env DOORBELL_TOKEN, else TOKEN= in
~/data/doorbell-bk7258/tcp_token.env (the value given to `tok set` over serial).
Host: --host, else env DOORBELL_HOST, default 192.168.2.89. Port: --port, else env DOORBELL_PORT,
default 2323. Options must come before the command, e.g. against the virtual device (sim/):

  python3 tools/dbcmd.py --host 127.0.0.1 --token sim sys info
"""
import os, socket, sys

DEFAULT_HOST = "192.168.2.89"
DEFAULT_PORT = 2323


def token(explicit=None):
    t = explicit or os.environ.get("DOORBELL_TOKEN")
    if t:
        return t
    try:
        for line in open(os.path.expanduser("~/data/doorbell-bk7258/tcp_token.env")):
            if line.startswith("TOKEN="):
                return line.strip().split("=", 1)[1]
    except FileNotFoundError:
        pass
    sys.exit("no token: use --token, set DOORBELL_TOKEN or ~/data/doorbell-bk7258/tcp_token.env")


def send(cmd, host=None, timeout=8.0, port=None, tok=None):
    host = host or os.environ.get("DOORBELL_HOST", DEFAULT_HOST)
    port = int(port or os.environ.get("DOORBELL_PORT", DEFAULT_PORT))
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.sendall(f"{token(tok)} {cmd}\n".encode())
            buf = b""
            while True:          # "ok\n" first, then any REPLY() text; device closes after ~100 ms
                try:
                    chunk = s.recv(512)
                except socket.timeout:
                    break
                if not chunk:
                    break
                buf += chunk
            return buf.decode(errors="replace").strip()
    except OSError as e:
        sys.exit(f"cannot reach {host}:{port} ({e.strerror or e}); is the device up and the token set?")


def parse_argv(argv):
    """Leading --host/--port/--token/--timeout options (also --opt=value), then the command words."""
    opts, i = {}, 0
    while i < len(argv) and argv[i].startswith("--"):
        key, _, val = argv[i][2:].partition("=")
        if key not in ("host", "port", "token", "timeout"):
            sys.exit(f"unknown option --{key}\n{__doc__}")
        if not _:
            i += 1
            if i >= len(argv):
                sys.exit(f"--{key} needs a value")
            val = argv[i]
        opts[key] = val
        i += 1
    return opts, argv[i:]


if __name__ == "__main__":
    options, words = parse_argv(sys.argv[1:])
    if not words:
        sys.exit(__doc__)
    print(send(" ".join(words), host=options.get("host"), port=options.get("port"), tok=options.get("token"),
               timeout=float(options.get("timeout", 8.0))))
