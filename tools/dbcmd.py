"""Send a command to the doorbell over the LAN (firmware v0.9.0+), no serial cable needed.

  python3 tools/dbcmd.py hook ring
  python3 tools/dbcmd.py ota http://<ha-ip>:8123/local/doorbell/doorbell-fw_0.9.1_app_pack.rbl
  python3 tools/dbcmd.py reboot
  python3 tools/dbcmd.py --dry-run sys info      # show the resolved host/port/token source, send nothing

Protocol: one TCP connection to port 2323, one line "<token> <command>\n", reply "ok" /
"unknown" / "denied", then the command's REPLY() text (v0.19+); the rest of the output goes to
the device's UART.

Settings (flag > environment > ~/.config/doorbell-bk7258/env, see scripts/env.example):
  host   --host,  else DOORBELL_HOST (required: no built-in default)
  port   --port,  else DOORBELL_PORT, default 2323
  token  --token, else DOORBELL_TOKEN, else the TOKEN= line of the file named by DOORBELL_TOKEN_FILE
         (the value given to `tok set` over serial)
Options must come before the command, e.g. against the virtual device (sim/):

  python3 tools/dbcmd.py --host 127.0.0.1 --token sim sys info
"""
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbenv  # noqa: E402

DEFAULT_PORT = 2323


def token_source(explicit=None):
    """(token, description of where it came from); token is None when nothing is configured."""
    if explicit:
        return explicit, "--token"
    t = dbenv.get("DOORBELL_TOKEN")
    if t:
        return t, "DOORBELL_TOKEN"
    path = dbenv.token_file()
    if path:
        try:
            for line in open(os.path.expanduser(path)):
                if line.startswith("TOKEN="):
                    return line.strip().split("=", 1)[1], "DOORBELL_TOKEN_FILE (%s)" % path
            return None, "DOORBELL_TOKEN_FILE (%s) has no TOKEN= line" % path
        except OSError as e:
            return None, "DOORBELL_TOKEN_FILE (%s) unreadable: %s" % (path, e.strerror or e)
    return None, "none"


def token(explicit=None):
    t, src = token_source(explicit)
    if t:
        return t
    sys.exit("no token (%s): use --token, set DOORBELL_TOKEN, or point DOORBELL_TOKEN_FILE at a file with TOKEN=..."
             % src)


def resolve_host(host=None):
    h = host or dbenv.get("DOORBELL_HOST")
    if not h:
        sys.exit("no device address: use --host, set DOORBELL_HOST, or put DOORBELL_HOST=<ip> in %s" % dbenv.ENV_FILE)
    return h


def resolve_port(port=None):
    return int(port or dbenv.get("DOORBELL_PORT") or DEFAULT_PORT)


def send(cmd, host=None, timeout=8.0, port=None, tok=None):
    host = resolve_host(host)
    port = resolve_port(port)
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


def dry_run(cmd, host=None, port=None, tok=None):
    """Print the resolved target and whether a token was found (never the token itself)."""
    h = host or dbenv.get("DOORBELL_HOST")
    t, src = token_source(tok)
    print("dry run: nothing sent")
    print("  host:    %s" % (h or "(unset: --host / DOORBELL_HOST)"))
    print("  port:    %d" % resolve_port(port))
    print("  token:   %s" % (("found via " + src) if t else ("NOT found (" + src + ")")))
    print("  command: %s" % cmd)
    return 0 if (h and t) else 1


def parse_argv(argv):
    """Leading --host/--port/--token/--timeout/--dry-run options (also --opt=value), then the command words."""
    opts, i = {}, 0
    while i < len(argv) and argv[i].startswith("--"):
        key, _, val = argv[i][2:].partition("=")
        if key == "dry-run":
            opts["dry-run"] = True
            i += 1
            continue
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
    if options.get("dry-run"):
        sys.exit(dry_run(" ".join(words), host=options.get("host"), port=options.get("port"), tok=options.get("token")))
    print(send(" ".join(words), host=options.get("host"), port=options.get("port"), tok=options.get("token"),
               timeout=float(options.get("timeout", 8.0))))
