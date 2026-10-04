"""TCP command channel: framing, token, reply strings, dbcmd.py compatibility."""
from __future__ import annotations

import re
import socket
import subprocess
import sys

from conftest import DBCMD, TOKEN


def raw(sim, line: bytes) -> bytes:
    with socket.create_connection(("127.0.0.1", sim.tcp), timeout=5) as s:
        s.sendall(line)
        buf = b""
        while True:
            try:
                chunk = s.recv(512)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
    return buf


def test_token_accepted_and_rejected(sim):
    assert sim.cmd("sys info").startswith("ok\n[sys] fw ")
    assert sim.cmd("sys info", token="wrong") == "denied"
    assert raw(sim, b"sysinfo\n") == b"denied\n"              # no space: the firmware cannot split the token
    assert raw(sim, b"%s\n" % TOKEN.encode()) == b"denied\n"


def test_unknown_command(sim):
    assert sim.cmd("frobnicate") == "unknown"
    assert raw(sim, b"%s nonsense arg\n" % TOKEN.encode()) == b"unknown\n"


def test_reply_formats(sim):
    r = sim.cmd("sys info")
    assert re.match(r"ok\n\[sys\] fw \S+ uptime \d+s \(\d+h\d\dm\) ip 127\.0\.0\.1 wifi 1 heap \d+ B reset 0x0 \(power-on\) "
                    r"light \d+ mV batt \d+ mV cam \d$", r), r
    assert re.match(r"ok\n\[pir\] on sens=1 \(GPIO25=1 GPIO26=0\) cooldown=10000 ms count=\d+ last=\d+ ms ago \(cooldown left \d+ ms\) "
                    r"edges=\d+ ignored=\d+ missed=0 GPIO24=0 GPIO50=1$", sim.cmd("pir status"))
    assert re.match(r"ok\n\[pwr\] batt \d+ mV \d+% \(adc15 \d+ mV x2\.00\) usb 1 chg 0 \| sleep off force off idle 60 s wake_pir off "
                    r"snap_min 0 \| sched 07:00-22:00 day 15 night 60 -> next (15|60) min \((day|night) by clock \d\d:\d\d\) "
                    r"\| up \d+ s hold \d+ s idle \d+ s busy [01] \| boot power-on$", sim.cmd("pwr status"))
    assert re.match(r"ok\n\[night\] auto 1 ring 1 hyst 400 \| light \d+ mV \(last \d+, dark > 1400\) cam [01] cam_dark 0 samples \d+ "
                    r"motion_left \d+ s \| white src 0x[0-9a-f]+ manual -1 out [01] \| ring manual -1 out 0 wifi 1 fx [01]$",
                    sim.cmd("night status"))
    assert re.match(r"ok\n\[cam\] open [01] \S+ \(mode auto\) frames \d+ h264 \d+ dropped \d+ last jpeg \d+ bytes \d+x\d+ seq \d+ age \d+ ms$",
                    sim.cmd("cam stat"))
    assert re.match(r"ok\n\[rtsp\] stopped port \d+ clients 0/3 \(0 playing\) in 0\.0 fps 0 kbps frames 0 key 0 drop busy 0 big 0 "
                    r"sps 0 pps 0 age 0 ms\r\n\[rtsp\] audio: voc off aec on \| clients 0 \| mic in 0 frames 0 B \| back in 0 pkts 0 B \| voc fails 0$",
                    sim.cmd("rtsp status"))
    # feat/duplex: the voc engine (SDK voice mode with AEC) behind the RTSP audio tracks
    assert re.match(r"ok\n\[talk\] idle engine raw \| speak: .* \| gain auto=0x1c micgain 0x3f max 30 s\r\n"
                    r"\[voc\] off aec on mic 0x3f spk 0x1c \| mic 0 cb 0 B \| spk 0 frames 0 B drop-tap 0 err 0 \| tap - 0 frames 0 late \| mictap - 0 ovr \| starts 0$",
                    sim.cmd("talk status"))
    assert sim.cmd("talk aec off") == "ok\n[talk] aec off (next voc start)"
    assert sim.cmd("talk aec on") == "ok\n[talk] aec on (next voc start)"
    assert sim.cmd("talk aec maybe") == "ok\n[talk] aec on|off"
    assert sim.cmd("talk vocmic 0x2d") == "ok\n[talk] voc mic gain 0x2d"
    assert sim.cmd("talk vocmic 0x40").startswith("ok\n[talk] vocmic 0..0x3f")
    assert sim.cmd("talk vocmic 0") == "ok\n[talk] voc mic gain 0x3f"
    assert sim.cmd("talk voc on").startswith("ok\n[talk] voc start -> 0 (manual")
    assert "engine voc" in sim.cmd("talk status") and "[voc] on aec on" in sim.cmd("talk status")
    assert sim.cmd("rtsp status").endswith("[rtsp] audio: voc on (manual) aec on | clients 0 | mic in 0 frames 0 B | back in 0 pkts 0 B | voc fails 0")
    assert sim.cmd("talk voc off") == "ok\n[talk] voc stopped (an RTSP audio client restarts it within 0.5 s)"
    assert "engine raw" in sim.cmd("talk status")
    assert re.match(r"ok\nADC9 raw=\d+ mv=30\d\d$", sim.cmd("adc 9"))
    assert sim.cmd("mem 28001000 2").startswith("ok\n[mem] 28001000: ")
    assert sim.cmd("sim nothing-like-this").startswith("ok\nsim press | motion |")


def test_argument_validation_matches_firmware(sim):
    assert sim.cmd("pir sens 7") == "ok\n[pir] sens must be 0-3 (stock used 0/1/2, default 1)"
    assert sim.cmd("pir cooldown 100") == "ok\n[pir] cooldown must be 500-600000 ms"
    assert sim.cmd("rtsp q 5 5") == "ok\n[rtsp] q IMB(10..290) PMB(4..1000) bits per macroblock; 720p = 3600 MB, 15 fps, IDR every 2 s"
    assert sim.cmd("led white maybe") == ("ok\nled white|ring on|off|auto   (on/off = manual override of the night light / "
                                           "ring pattern, auto = release)")
    assert sim.cmd("cam mode mpeg") == "ok\ncam mode auto|jpeg|h264"
    assert sim.cmd("talk gain 0x40") == "ok\n[talk] gain 0..0x3f or auto (0x1c = loudest clean level, above 0x1c the amplifier clips)"
    assert sim.cmd("tok") == "ok"          # os_printf-only command: nothing comes back over TCP
    assert sim.cmd("pin all") == "ok"


def test_settings_round_trip_and_persist(sim):
    assert sim.cmd("pir cooldown 3000") == "ok\n[pir] cooldown 3000 ms"
    assert "cooldown=3000 ms" in sim.cmd("pir status")
    assert sim.state_json()["kv"]["db_pir_cd"] == "3000"
    assert sim.cmd("ring dark 800") == "ok\n[ring] dark threshold 800 mV"
    assert sim.state_json()["kv"]["db_dark_mv"] == "800"
    sim.cmd("ring dark 1400")
    sim.cmd("pir cooldown 10000")


def test_hook_set_persists_in_state_file(sim, sink):
    original = sink.hook()
    other = sink.hook("/api/webhook/other")
    assert sim.cmd("hook set " + other) == "ok\n[hook] %s  (hook set HOST:PORT/PATH | test | ring | motion)" % other
    assert sim.state_json()["kv"]["db_hook"] == other
    mark = sink.mark()
    sim.cmd("hook test")
    assert sink.wait_for("test", 5, since=mark)["_path"] == "/api/webhook/other"
    sim.cmd("hook set " + original)
    assert sim.state_json()["kv"]["db_hook"] == original


def test_dbcmd_cli_options_and_env(sim):
    out = subprocess.run([sys.executable, str(DBCMD), "--host", "127.0.0.1", "--port", str(sim.tcp), "--token", TOKEN, "sys", "info"],
                         capture_output=True, text=True, timeout=15)
    assert out.returncode == 0 and out.stdout.startswith("ok\n[sys] fw "), out
    out = subprocess.run([sys.executable, str(DBCMD), "hook"], capture_output=True, text=True, timeout=15,
                         env={"PATH": "/usr/bin:/bin", "DOORBELL_HOST": "127.0.0.1", "DOORBELL_PORT": str(sim.tcp), "DOORBELL_TOKEN": TOKEN})
    assert out.returncode == 0 and out.stdout.startswith("ok\n[hook] "), out
    out = subprocess.run([sys.executable, str(DBCMD), "--port=%d" % sim.tcp, "--host=127.0.0.1", "--token=nope", "sys", "info"],
                         capture_output=True, text=True, timeout=15)
    assert out.stdout.strip() == "denied"


def test_control_port_only_takes_sim_commands(sim):
    assert sim.ctl_cmd("sim status").startswith("ok\n[sim] awake fw ")
    assert sim.ctl_cmd("sys info") == "unknown"
