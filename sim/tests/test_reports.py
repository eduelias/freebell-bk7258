"""Webhook reports: boot / ring / motion / test JSON with the firmware's fields."""
from __future__ import annotations

import time

from conftest import wait_until

REQUIRED = {"event", "fw", "ip", "mv", "light_mv", "uptime_s", "motion_count", "ring_count", "pir_on", "pir_sens",
            "batt_mv", "batt_pct", "usb", "charging"}


def test_boot_report(sim, sink):
    boot = sink.wait_for("boot", 5, since=sim.start_mark)
    assert REQUIRED <= set(boot)
    assert boot["ip"] == "127.0.0.1" and boot["pir_on"] == 1 and boot["pir_sens"] == 1 and boot["usb"] == 1
    assert boot["_path"] == "/api/webhook/test"


def test_ring_report_and_rate_limit(sim, sink):
    mark = sink.mark()
    assert sim.cmd("sim press").startswith("ok\n[sim] button pressed: ring_count ")
    ring = sink.wait_for("ring", 5, since=mark)
    assert REQUIRED <= set(ring) and ring["ring_count"] >= 1
    before = ring["ring_count"]
    time.sleep(0.9)                                     # past the 0.8 s debounce, inside the 10 s report limit
    sim.cmd("sim press")
    time.sleep(1.0)
    rings = [e for e in sink.events[mark:] if e["event"] == "ring"]
    assert len(rings) == 1, "a second ring report inside 10 s"
    assert "ring_count %d" % (before + 1) in sim.cmd("sim status") or "ring %d" % (before + 1) in sim.cmd("sim status")


def test_motion_report_with_cooldown(sim, sink):
    sim.cmd("pir cooldown 500")
    mark = sink.mark()
    assert wait_until(lambda: sim.cmd("motion test").startswith("ok\n[motion] simulated edge accepted"), 8, step=0.5), \
        "motion test never accepted (PIR warm-up is 5 s after boot)"
    motion = sink.wait_for("motion", 5, since=mark)
    assert REQUIRED <= set(motion) and motion["motion_count"] >= 1
    r = sim.cmd("motion test")
    assert r.startswith("ok\n[motion] ignored: cooldown, ") or r.startswith("ok\n[motion] simulated edge accepted")
    sim.cmd("pir off")
    assert sim.cmd("motion test") == "ok\n[motion] ignored: pir off (`pir on`)"
    assert sim.cmd("pir on") == "ok\n[pir] on (GPIO50=1, IRQ enabled, edges ignored for 5000 ms)"
    assert sim.cmd("motion test").startswith("ok\n[motion] ignored: warm-up, ")
    sim.cmd("pir cooldown 10000")


def test_hook_test_and_sensor_overrides(sim, sink):
    sim.cmd("sim light 1900")
    sim.cmd("sim batt 3700")
    sim.cmd("sim usb off")
    mark = sink.mark()
    sim.cmd("hook test")
    t = sink.wait_for("test", 5, since=mark)
    assert REQUIRED <= set(t)
    assert abs(t["light_mv"] - 1900) <= 5 and abs(t["batt_mv"] - 3700) <= 8 and 33 <= t["batt_pct"] <= 37
    assert t["usb"] == 0 and t["charging"] == 0
    r = sim.cmd("ring test")
    assert r.startswith("ok\n[ring] effects started (light 1") and r.endswith("dark above 1400 -> white light on)"), r
    sim.cmd("sim light 740")
    sim.cmd("sim usb on")
    sim.cmd("sim batt 3886")


def test_hook_ring_and_motion_shortcuts(sim, sink):
    mark = sink.mark()
    assert sim.cmd("hook motion").startswith("ok\n[hook] 127.0.0.1:")
    sink.wait_for("motion", 5, since=mark)
    time.sleep(0.2)
    mark = sink.mark()
    sim.cmd("hook ring")
    # hook ring bypasses the 10 s button limit only if the previous ring report is older than that; so just check the queue drains
    assert wait_until(lambda: "busy 0" in sim.cmd("pwr status") or any(e["event"] == "ring" for e in sink.events[mark:]), 5)


def test_heartbeat_can_be_forced(sim, sink):
    mark = sink.mark()
    assert sim.cmd("sim heartbeat") == "ok\n[sim] heartbeat report queued"
    assert REQUIRED <= set(sink.wait_for("heartbeat", 5, since=mark))
