"""0.28.1 boot-loop protection (src/rescue.c): crash resets are counted, three in a row start rescue mode."""
from __future__ import annotations

from conftest import port_open, wait_until


def crash_and_wait(sim, sink, expect: str) -> dict:
    mark = sink.mark()
    assert sim.cmd("rescue crash").startswith("ok\n[rescue] crash test")
    assert wait_until(lambda: not port_open(sim.tcp), 5), "no reset after the crash test"
    return sink.wait_for(expect, 10, since=mark)


def test_three_crashes_start_rescue_mode(fast_sim, sink):
    sim = fast_sim
    assert "normal" in sim.cmd("rescue")
    crash_and_wait(sim, sink, "boot")
    boot = crash_and_wait(sim, sink, "boot")
    assert boot["fw"]
    assert "2 crash reset(s) in a row" in sim.cmd("rescue")
    rescue = crash_and_wait(sim, sink, "rescue")
    assert rescue["crashes"] == 3 and rescue["reset_reason"] == 0xE and rescue["forced"] == 0
    st = sim.cmd("rescue")
    assert "RESCUE MODE" in st and "3 crash reset(s)" in st
    assert "not available in rescue mode" in sim.cmd("cam stat")
    assert sim.cmd("hook show").startswith("ok\n[hook]")          # on the rescue list
    assert "crash count cleared" in sim.cmd("rescue off")
    mark = sink.mark()
    sim.cmd("reboot")
    assert sink.wait_for("boot", 10, since=mark)                   # a normal start again
    assert "normal" in sim.cmd("rescue") and "0 crash reset(s)" in sim.cmd("rescue")


def test_forced_rescue_and_ota_clears(fast_sim, sink):
    sim = fast_sim
    assert "next boot starts in rescue mode" in sim.cmd("rescue on")
    mark = sink.mark()
    sim.cmd("reboot")
    assert sink.wait_for("rescue", 10, since=mark)["forced"] == 1
    assert "forced on" in sim.cmd("rescue")
    sim.cmd("ota http://127.0.0.1:9/none.rbl")                     # an OTA attempt clears count + forced flag
    assert "forced off" in sim.cmd("rescue")
