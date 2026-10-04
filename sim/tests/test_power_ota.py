"""Deep sleep / wake, reboot and OTA: the ports vanish and come back, with the right reports."""
from __future__ import annotations

import functools
import http.server
import os
import threading

from conftest import port_open, wait_until


def test_fresh_device_status_page_is_503_until_the_camera_opened(fast_sim):
    status, _, body = fast_sim.get("/")
    assert status == 503 and body == b"camera not open"
    status, _, _ = fast_sim.get("/snapshot.jpg")
    assert status == 200
    status, _, _ = fast_sim.get("/")
    assert status == 200


def test_sleep_rtc_wake_cycle(fast_sim, sink):
    sim = fast_sim
    assert sim.cmd("pwr snap_min 1") == "ok\n[pwr] rtc wake every 1 min (used only with `pwr sched off`; the schedule is on)"
    assert sim.cmd("pwr sched off").startswith("ok\n[pwr] sched off (fixed snap_min 1) -> next rtc wake in 1 min (fixed snap_min)")
    r = sim.cmd("pwr sleepnow")
    assert r == "ok\n[pwr] sleeping in 10 s: wake on button, usb plug, uart rx, rtc in 1 min (cancel: pwr sleep off)"
    mark = sink.mark()
    assert wait_until(lambda: not port_open(sim.tcp), 5), "TCP port still open after sleepnow (10 s at 20x)"
    assert not port_open(sim.http)
    assert sim.ctl_cmd("sim status").startswith("ok\n[sim] asleep ")
    assert wait_until(lambda: port_open(sim.tcp), 10), "device did not wake (60 s rtc at 20x)"
    boot = sink.wait_for("boot", 5, since=mark)
    wake = sink.wait_for("wake", 5, since=mark)
    assert wake["reason"] == "rtc" and boot["uptime_s"] <= wake["uptime_s"] <= 5
    assert "boot deepsleep-rtc wake=rtc" in sim.cmd("pwr status")
    assert "reset 0x4 (deepsleep-rtc)" in sim.cmd("sys info")
    assert sim.state_json()["kv"]["db_pwr_snap"] == "1"


def test_sleep_button_wake_rings(fast_sim, sink):
    sim = fast_sim
    sim.cmd("pwr sched off")
    sim.cmd("pwr snap_min 10")
    sim.cmd("pwr sleepnow")
    assert wait_until(lambda: not port_open(sim.tcp), 5)
    mark = sink.mark()
    assert sim.ctl_cmd("sim press") == "ok\n[sim] button pressed while asleep: waking (reason button)"
    assert wait_until(lambda: port_open(sim.tcp), 10)
    assert sink.wait_for("wake", 5, since=mark)["reason"] == "button"
    assert sink.wait_for("ring", 5, since=mark)["ring_count"] == 1
    assert "boot deepsleep-gpio wake=button" in sim.cmd("pwr status")


def test_pwr_sleep_off_cancels_sleepnow(fast_sim):
    sim = fast_sim
    sim.cmd("pwr sleepnow")
    assert "sleepnow pending" in sim.cmd("pwr status")
    assert sim.cmd("pwr sleep off") == "ok\n[pwr] automatic sleep off"
    assert "sleepnow pending" not in sim.cmd("pwr status")
    assert wait_until(lambda: not port_open(sim.tcp), 1.5) is False, "device slept although sleepnow was cancelled"


def test_reboot_drops_ports_and_reposts_boot(fast_sim, sink):
    sim = fast_sim
    mark = sink.mark()
    assert sim.cmd("reboot") == "ok"
    assert wait_until(lambda: not port_open(sim.tcp), 3)
    assert wait_until(lambda: port_open(sim.tcp), 10)
    sink.wait_for("boot", 5, since=mark)
    assert "reset 0x1 (reboot)" in sim.cmd("sys info")


def test_ota_bumps_the_version(fast_sim, sink, tmp_path):
    sim = fast_sim
    (tmp_path / "doorbell-fw_9.9.9_app_pack.rbl").write_bytes(os.urandom(100 * 1024))
    (tmp_path / "empty.rbl").write_bytes(b"")
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(tmp_path))
    handler.log_message = lambda *a: None  # type: ignore[attr-defined]
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        base = "http://127.0.0.1:%d/" % srv.server_address[1]
        old = sim.cmd("sys info").split(" ")[2]
        mark = sink.mark()
        assert sim.cmd("ota " + base + "doorbell-fw_9.9.9_app_pack.rbl") == "ok"
        assert wait_until(lambda: not port_open(sim.tcp), 5), "no reboot after the OTA download"
        assert wait_until(lambda: port_open(sim.tcp), 10), "device did not come back (35 s at 20x)"
        assert sink.wait_for("boot", 5, since=mark)["fw"] == "9.9.9" != old
        assert "fw 9.9.9 " in sim.cmd("sys info") and "reset 0x18 (ota)" in sim.cmd("sys info")
        assert sim.state_json()["fw"] == "9.9.9"
        assert sim.cmd("ota " + base + "empty.rbl") == "ok"           # rejected: no reboot, version unchanged
        assert wait_until(lambda: not port_open(sim.tcp), 1.5) is False
        assert sim.cmd("ota " + base + "doorbell-fw_1.2.3_app_pack.bin") == "ok"   # not an .rbl: rejected
        assert wait_until(lambda: not port_open(sim.tcp), 1.5) is False
        assert "fw 9.9.9 " in sim.cmd("sys info")
    finally:
        srv.shutdown()
        srv.server_close()
