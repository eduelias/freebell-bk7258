"""fw 0.25: volume, RAM log, GET /cmd + /log with the token, the RTC schedule and the policy pull."""
from __future__ import annotations

import functools
import http.server
import json
import threading
import urllib.parse

from conftest import TOKEN, wait_until


def test_vol_maps_linearly_onto_the_dac_gain(sim):
    assert sim.cmd("vol") == "ok\n[vol] 100% -> dac gain 0x1c (chime + talk auto; `spk cfg gain N` overrides raw)"
    assert sim.cmd("vol 60") == "ok\n[vol] 60% -> dac gain 0x11 (chime + talk auto; `spk cfg gain N` overrides raw)"
    assert sim.state_json()["kv"]["db_vol"] == "60"
    assert "gain 0x11 (vol)" in sim.cmd("spk cfg")                       # the chime path follows the volume
    assert "gain auto=0x11" in sim.cmd("talk status")                     # talk `gain auto` too
    assert sim.cmd("vol 0").endswith("[vol] 0% -> dac gain 0x0 (chime + talk auto; `spk cfg gain N` overrides raw)")
    assert sim.cmd("vol 101").startswith("ok\nvol [0-100]")
    assert sim.cmd("vol x").startswith("ok\nvol [0-100]")
    assert "gain 0x1c (raw override)" in sim.cmd("spk cfg gain 0x1c")    # `spk cfg gain` is a raw override...
    assert "gain auto=0x1c" in sim.cmd("talk status")
    assert "gain 0x0 (vol)" in sim.cmd("spk cfg gain auto")              # ...until released
    sim.cmd("vol 100")


def test_log_ring_and_log_command(sim):
    sim.cmd("vol 100")
    r = sim.cmd("log 300")
    assert r.startswith("ok\n") and "[vol] 100%" in r and "[tcp] vol 100" in r
    assert len(r) <= 4 + 300
    assert sim.cmd("log stat").startswith("ok\n[log] ") and "in the 8192-byte ring" in sim.cmd("log stat")
    full = sim.cmd("log 9999")                                            # capped at 600 bytes
    assert len(full) <= 4 + 600


def test_cmd_endpoint_needs_the_token(sim):
    status, headers, body = sim.get("/cmd?t=%s&c=%s" % (TOKEN, urllib.parse.quote_plus("sys info")))
    assert status == 200 and headers["Content-Type"] == "text/plain" and body.startswith(b"[sys] fw ")
    status, _, body = sim.get("/cmd?c=sys+info")
    assert status == 403 and body == b"denied\n"
    status, _, body = sim.get("/cmd?t=wrong&c=sys+info")
    assert status == 403
    status, _, body = sim.get("/cmd?t=%s" % TOKEN)
    assert status == 400
    status, _, body = sim.get("/cmd?t=%s&c=frobnicate" % TOKEN)
    assert status == 404 and body == b"unknown command\n"
    status, _, body = sim.get("/cmd?t=%s&c=vol+70" % TOKEN)             # url-encoded command with an argument
    assert status == 200 and body.startswith(b"[vol] 70% -> dac gain 0x14")
    sim.cmd("vol 100")
    status, headers, body = sim.get("/log?t=%s" % TOKEN)
    assert status == 200 and headers["Content-Type"] == "text/plain" and b"[http] cmd: vol 70" in body and len(body) <= 8192
    status, _, _ = sim.get("/log")
    assert status == 403


def test_sched_command_and_status(sim):
    r = sim.cmd("pwr sched 06:30 23:00 10 45")
    assert r.startswith("ok\n[pwr] sched 06:30-23:00 day 10 night 45 -> next ")
    assert sim.state_json()["kv"]["db_pwr_sched"] == "06:30 23:00 10 45"
    assert "sched 06:30-23:00 day 10 night 45 -> next" in sim.cmd("pwr status")
    assert sim.cmd("pwr sched 25:00 23:00 10 45").startswith("ok\npwr sched DAY_START DAY_END")
    assert sim.cmd("pwr sched off").startswith("ok\n[pwr] sched off (fixed snap_min 0) -> next rtc wake in 360 min (fixed snap_min (keepalive))")
    assert sim.cmd("pwr snap_min 30") == "ok\n[pwr] rtc wake every 30 min"
    assert "-> next rtc wake in 30 min (fixed snap_min)" in sim.cmd("pwr sched")
    assert sim.cmd("pwr sched on").startswith("ok\n[pwr] sched 06:30-23:00 day 10 night 45 -> next ")
    assert "(used only with `pwr sched off`; the schedule is on)" in sim.cmd("pwr snap_min 0")
    # battery derating: < 30 % doubles, < 15 % = 180 min and the PIR wake is forced off
    sim.cmd("sim usb off")
    sim.cmd("pwr wake_pir on")
    sim.cmd("sim batt 3650")                                              # ~22 %
    assert ": x2)" in sim.cmd("pwr status")
    sim.cmd("sim batt 3560")                                              # ~7 %
    st = sim.cmd("pwr status")
    assert "wake_pir on (FORCED OFF: battery)" in st and "180 min, pir wake off)" in st
    sim.cmd("sim batt 3886")
    sim.cmd("sim usb on")
    sim.cmd("pwr wake_pir off")
    sim.cmd("pwr sched 07:00 22:00 15 60")


def test_policy_pull_applies_whitelisted_keys(fast_sim, sink, tmp_path):
    sim = fast_sim
    policy = {"rev": 1759600000, "volume": 55, "pir_sens": 2, "wake_pir": 1, "dark_mv": 1500, "night_auto": 0, "keep_s": 45,
              "sd_auto": 1, "day_start": "08:00", "day_end": "21:30", "day_min": 20, "night_min": 90,
              "sleep": 0, "hook": "evil:1/x", "token": "nope"}                 # the last three are not in the whitelist
    (tmp_path / "policy.json").write_text(json.dumps(policy))
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(tmp_path))
    handler.log_message = lambda *a: None  # type: ignore[attr-defined]
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        url = "http://127.0.0.1:%d/policy.json" % srv.server_address[1]
        boot = sink.wait_for("boot", 5, since=sim.start_mark)
        assert boot["policy"] == "error" and boot["policy_rev"] == ""      # auto URL = the webhook host, which has no policy.json
        assert "sleep_min" in boot and "wake_pir" in boot
        r = sim.cmd("pwr policy " + url)
        assert r.startswith("ok\n[policy] %s | last: error" % url)
        r = sim.cmd("pwr policy pull")
        assert r.startswith("ok\n[policy] %s | last: applied rev 1759600000 (7 changed 1 same)" % url), r   # sd_auto 1 is the default
        assert sim.cmd("vol") == "ok\n[vol] 55% -> dac gain 0xf (chime + talk auto; `spk cfg gain N` overrides raw)"
        assert "sens=2" in sim.cmd("pir status")
        st = sim.cmd("pwr status")
        assert "wake_pir on" in st and "sched 08:00-21:30 day 20 night 90" in st and "sleep off" in st
        assert "dark > 1500" in sim.cmd("night status") and "auto 0" in sim.cmd("night status")
        kv = sim.state_json()["kv"]
        assert kv["db_vol"] == "55" and kv["db_sd_keep"] == "45" and kv["db_pwr_sched"] == "08:00 21:30 20 90"
        assert kv.get("db_hook", "").startswith("127.0.0.1:") and kv.get("db_token", TOKEN) == TOKEN
        # second pull: nothing to change; a reboot refetches before the boot report
        assert "(0 changed 8 same)" in sim.cmd("pwr policy pull")
        mark = sink.mark()
        sim.cmd("reboot")
        boot = sink.wait_for("boot", 10, since=mark)
        assert boot["policy"] == "applied" and boot["policy_rev"] == "1759600000"
        assert wait_until(lambda: sim.cmd("vol").startswith("ok\n[vol] 55%"), 5)
        # `pwr sched off` on the device wins over the schedule keys
        sim.cmd("pwr sched off")
        assert "(0 changed 7 same)" in sim.cmd("pwr policy pull")
        assert sim.cmd("pwr policy off").startswith("ok\n[policy] off | last: applied rev 1759600000")
        assert sim.cmd("pwr policy pull").startswith("ok\n[policy] off | last: none (off)")
        assert sim.cmd("pwr policy auto").startswith("ok\n[policy] http://127.0.0.1:")
    finally:
        srv.shutdown()
        srv.server_close()
