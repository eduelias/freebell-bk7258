#!/usr/bin/env python3
"""Write /config/www/doorbell/policy.json for the doorbell's boot-time policy pull (fw 0.25).

Run by shell_command.doorbell_policy_write inside the Home Assistant core container with the
helper states as arguments (in this order):

  doorbell_policy.py VOLUME PIR_SENS PIR_WAKE DAY_MIN NIGHT_MIN DARK_MV NIGHT_LIGHT

Values are what HA's templates render: numbers ("60.0"), "0"/"1"/"2", "on"/"off". The file is a
flat JSON object with the keys the firmware whitelists (policy.c) plus "rev" = the write time as
a Unix timestamp, so the boot report's policy_rev shows which file the device applied. Written
atomically (tmp + rename), world-readable so HA serves it at /local/doorbell/policy.json.
"""
import json
import os
import sys
import time

OUT = os.environ.get("DOORBELL_POLICY_FILE", "/config/www/doorbell/policy.json")
DAY_START = os.environ.get("DOORBELL_DAY_START", "07:00")
DAY_END = os.environ.get("DOORBELL_DAY_END", "22:00")


def num(s, lo, hi, default):
    try:
        v = int(float(s))
    except (TypeError, ValueError):
        return default
    return min(hi, max(lo, v))


def flag(s, default):
    s = str(s).strip().lower()
    return 1 if s in ("on", "1", "true") else 0 if s in ("off", "0", "false") else default


def main(argv):
    if len(argv) != 7:
        sys.exit("usage: doorbell_policy.py VOLUME PIR_SENS PIR_WAKE DAY_MIN NIGHT_MIN DARK_MV NIGHT_LIGHT")
    volume, pir_sens, pir_wake, day_min, night_min, dark_mv, night_light = argv
    policy = {
        "rev": int(time.time()),
        "day_start": DAY_START,
        "day_end": DAY_END,
        "day_min": num(day_min, 1, 1440, 15),
        "night_min": num(night_min, 1, 1440, 60),
        "wake_pir": flag(pir_wake, 1),
        "pir_sens": num(pir_sens, 0, 2, 0),
        "dark_mv": num(dark_mv, 100, 4000, 1400),
        "volume": num(volume, 0, 100, 100),
        "night_auto": flag(night_light, 1),
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    tmp = OUT + ".tmp"
    with open(tmp, "w") as f:
        json.dump(policy, f, separators=(",", ":"))
        f.write("\n")
    os.chmod(tmp, 0o644)
    os.replace(tmp, OUT)
    print(json.dumps(policy))


if __name__ == "__main__":
    main(sys.argv[1:])
