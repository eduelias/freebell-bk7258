"""Shared settings loader for the doorbell tools (standard library only).

One optional dotenv file, ``~/.config/doorbell-bk7258/env`` (``KEY=value`` lines, ``#`` comments,
optional single or double quotes, ``$HOME``/``~`` expanded in unquoted and double-quoted values),
plus the real environment. Precedence, highest first:

    command-line flag  >  environment variable  >  env file  >  built-in default

Keys (see scripts/env.example): DOORBELL_HOST, DOORBELL_TOKEN, DOORBELL_TOKEN_FILE, SERIAL_PORT,
FW_DIR, OUT, SDK, AVDK, HA_URL, HA_TOKEN_FILE, OTA_URL_BASE, OTA_PUBLISH, BUILD.
``scripts/env.sh`` is the bash counterpart for the shell scripts.
"""
import glob
import os
import sys

ENV_FILE = "~/.config/doorbell-bk7258/env"

DEFAULTS = {
    "FW_DIR": "~/data/doorbell-bk7258/firmware",
    "SDK": "~/reps/oss/bk_idk",
    "AVDK": "~/reps/oss/bk_avdk",
    "HA_URL": "http://homeassistant.local:8123",
    "HA_TOKEN_FILE": "~/.ha_token",
    "BUILD": "build.sh",
    "DOORBELL_PORT": "2323",
}

SERIAL_GLOBS = ("/dev/cu.usbserial-*", "/dev/ttyUSB*", "/dev/ttyACM*")

_cache = None


def _unquote(raw):
    """Shell-like value parsing: strip one pair of quotes; expand $VAR/~ unless single-quoted."""
    v = raw.strip()
    if len(v) >= 2 and v[0] == v[-1] == "'":
        return v[1:-1]
    if len(v) >= 2 and v[0] == v[-1] == '"':
        v = v[1:-1].replace('\\"', '"').replace("\\\\", "\\").replace("\\$", "\0")
        return os.path.expanduser(os.path.expandvars(v)).replace("\0", "$")
    if " #" in v:                      # trailing comment on an unquoted value
        v = v.split(" #", 1)[0].rstrip()
    return os.path.expanduser(os.path.expandvars(v))


def read_env_file(path=ENV_FILE):
    """Parse a dotenv file into a dict; a missing file is an empty dict."""
    out = {}
    try:
        lines = open(os.path.expanduser(path), encoding="utf-8").read().splitlines()
    except OSError:
        return out
    for line in lines:
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("export "):
            s = s[7:].lstrip()
        key, eq, val = s.partition("=")
        key = key.strip()
        if not eq or not key or not key.replace("_", "").isalnum():
            continue
        out[key] = _unquote(val)
    return out


def load_env(path=ENV_FILE):
    """File values overlaid with the real environment (environment wins). Cached per process."""
    global _cache
    if _cache is None or path != ENV_FILE:
        merged = read_env_file(path)
        merged.update(os.environ)
        if path != ENV_FILE:
            return merged
        _cache = merged
    return _cache


def get(key, default=None):
    """Value for KEY, else the given default, else the built-in default, else None. Paths expand ~."""
    v = load_env().get(key)
    if v is None or v == "":
        v = DEFAULTS.get(key) if default is None else default
    if isinstance(v, str) and v.startswith("~"):
        v = os.path.expanduser(v)
    return v


def require(key, hint=None):
    """Like get(), but exit with a clear message when the key is not configured anywhere."""
    v = get(key)
    if not v:
        sys.exit("%s is not set: pass the matching flag, export %s, or put %s=... in %s%s"
                 % (key, key, key, ENV_FILE, (" (%s)" % hint) if hint else ""))
    return v


def serial_port(explicit=None, required=True):
    """UART adapter: --port value, else SERIAL_PORT, else the first /dev/cu.usbserial-*, /dev/ttyUSB*,
    /dev/ttyACM* match. With required=False a missing adapter returns None instead of exiting."""
    if explicit:
        return explicit
    p = get("SERIAL_PORT")
    if p:
        return p
    for pat in SERIAL_GLOBS:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    if required:
        sys.exit("no serial adapter found (%s): pass --port or set SERIAL_PORT" % ", ".join(SERIAL_GLOBS))
    return None


def token_file():
    """Path of the file holding `TOKEN=...` (DOORBELL_TOKEN_FILE), or None when not configured."""
    return get("DOORBELL_TOKEN_FILE")


if __name__ == "__main__":
    env = load_env()
    keys = ["DOORBELL_HOST", "DOORBELL_PORT", "DOORBELL_TOKEN_FILE", "SERIAL_PORT", "FW_DIR", "OUT", "SDK", "AVDK",
            "HA_URL", "HA_TOKEN_FILE", "OTA_URL_BASE", "OTA_PUBLISH", "BUILD"]
    print("env file: %s (%s)" % (os.path.expanduser(ENV_FILE), "found" if os.path.exists(os.path.expanduser(ENV_FILE)) else "absent"))
    for k in keys:
        print("%-20s %s" % (k, get(k) if get(k) is not None else "(unset)"))
    print("%-20s %s" % ("DOORBELL_TOKEN", "set" if env.get("DOORBELL_TOKEN") else "(unset)"))
    print("%-20s %s" % ("serial port", serial_port(required=False) or "(none found)"))
