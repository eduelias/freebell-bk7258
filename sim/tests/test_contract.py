"""Contract: every command the firmware registers must be handled by the simulator."""
from __future__ import annotations

import re
import sys

from conftest import REPO, SIM, load_module

sys.path.insert(0, str(SIM.parent))
vdb = load_module("virtual_doorbell", SIM)

CMD_ENTRY = re.compile(r'^\s*\{\s*"([a-z0-9_]+)"\s*,\s*"', re.M)


def firmware_commands() -> dict[str, set[str]]:
    """Command names per source file, from every `struct cli_command` array in project/main."""
    found: dict[str, set[str]] = {}
    for src in sorted((REPO / "project" / "main").glob("*.c")):
        text = src.read_text(errors="replace")
        for m in re.finditer(r"struct cli_command\s+\w+\[\]\s*=\s*\{(.*?)^\};", text, re.S | re.M):
            names = set(CMD_ENTRY.findall(m.group(1)))
            if names:
                found.setdefault(src.name, set()).update(names)
    return found


def test_firmware_command_table_is_parsed():
    found = firmware_commands()
    assert "cmd.c" in found, "s_doorbell_commands[] not found in cmd.c"
    assert {"hook", "pir", "pwr", "cam", "sys"} <= found["cmd.c"]


def test_simulator_covers_every_firmware_command():
    missing = {}
    for src, names in firmware_commands().items():
        gap = names - set(vdb.Device.COMMANDS)
        if gap:
            missing[src] = sorted(gap)
    assert not missing, "firmware commands without a simulator handler: %s" % missing


def test_fw_version_follows_the_source():
    m = re.search(r'#define FW_VERSION "([^"]+)"', (REPO / "project" / "main" / "version.h").read_text())
    assert m and vdb.fw_version_from_source() == m.group(1)


def test_report_json_has_the_firmware_fields():
    """The field list of report_ex() in net.c + power_json() in power.c + policy_json() in policy.c (boot report)."""
    src = "".join((REPO / "project" / "main" / f).read_text() for f in ("net.c", "power.c", "policy.c"))
    fw_fields = set(re.findall(r'\\"([a-z_]+)\\":', src))
    args = vdb.parse_args(["--state", "/dev/null", "--ip", "127.0.0.1"])
    dev = vdb.Device.__new__(vdb.Device)   # no sockets: only the JSON formatter is exercised
    dev.args, dev.clock, dev.store = args, vdb.SimClock(), None
    dev.fw, dev.ip, dev.light_mv, dev.light_chan, dev.batt_mv, dev.usb, dev.charging = "1.0", "127.0.0.1", 740, 14, 3886, 1, 0
    dev.pwr_bchan, dev.pwr_bratio, dev.boot_ms, dev.motion_count, dev.ring_count, dev.pir_on, dev.pir_sens = 15, 200, 0, 0, 0, True, 1
    dev.sched_on, dev.day_start, dev.day_end, dev.day_min, dev.night_min, dev.pwr_snap_min = True, 7 * 60, 22 * 60, 15, 60, 0
    dev.pwr_wake_pir, dev.low_batt, dev.policy_result, dev.policy_rev = False, False, "none", ""
    sim_fields = set(re.findall(r'"([a-z_]+)":', dev.report_json("boot")))
    assert fw_fields - {"reason"} <= sim_fields, "report fields missing in the simulator: %s" % (fw_fields - sim_fields)
