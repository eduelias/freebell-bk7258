"""HTTP: snapshot, MJPEG stream, status page, /sim controls, feat/talk routes."""
from __future__ import annotations

import http.client
import io
import json
import struct
import time

from conftest import wait_until

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None


def test_snapshot_is_a_720p_jpeg(sim):
    status, headers, body = sim.get("/snapshot.jpg")
    assert status == 200 and headers["Content-Type"] == "image/jpeg" and headers["Connection"] == "close"
    assert int(headers["Content-Length"]) == len(body) > 1000
    assert body[:2] == b"\xff\xd8" and body[-2:] == b"\xff\xd9"
    if Image is not None:
        img = Image.open(io.BytesIO(body))
        img.load()
        assert img.size == (1280, 720)
    status2, _, body2 = sim.get("/snapshot")          # the firmware prefix-matches "GET /snapshot"
    assert status2 == 200 and body2[:2] == b"\xff\xd8"


def test_status_page(sim):
    status, headers, body = sim.get("/")
    assert status == 200 and headers["Content-Type"] == "text/html"
    assert body.startswith(b"<html><body>doorbell ") and b"cam open=1 frames=" in body and b"/snapshot.jpg" in body
    status, _, body = sim.get("/anything-else")
    assert status == 200 and b"doorbell" in body


def test_stream_yields_boundaries(sim):
    c = http.client.HTTPConnection("127.0.0.1", sim.http, timeout=10)
    c.request("GET", "/stream")
    r = c.getresponse()
    assert r.status == 200 and r.getheader("Content-Type") == "multipart/x-mixed-replace; boundary=frame"
    buf, deadline = b"", time.monotonic() + 8
    while buf.count(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ") < 3 and time.monotonic() < deadline:
        buf += r.read1(65536)
    c.close()
    assert buf.count(b"--frame\r\n") >= 3, buf[:200]
    first = buf.find(b"\r\n\r\n") + 4
    assert buf[first:first + 2] == b"\xff\xd8"


def test_sim_controls_over_http(sim, sink):
    mark = sink.mark()
    status, _, body = sim.get("/sim/press")
    assert status == 200 and body.startswith(b"[sim] button pressed")
    assert sink.wait_for("ring", 5, since=mark)["ring_count"] >= 1
    status, _, body = sim.get("/sim/light/1800")
    assert status == 200 and body == b"[sim] light sensor 1800 mV (dark, dark above 1400)\n"
    status, _, body = sim.get("/sim/light%20740")
    assert status == 200 and b"740 mV (day" in body
    status, _, body = sim.get("/sim/nope")
    assert status == 404
    status, _, body = sim.get("/sim/status")
    assert status == 200 and body.startswith(b"[sim] awake fw ")


def test_talk_routes(sim):
    # the bell of the previous test's button press keeps the speaker busy for ~4 s (as on the device)
    assert wait_until(lambda: sim.cmd("spk").endswith("(busy 0)"), 8, step=0.25), "speaker still busy"
    status, headers, body = sim.get("/speak?tone=1000&ms=120")
    assert status == 200 and headers["Content-Type"] == "application/json" and headers["Access-Control-Allow-Origin"] == "*"
    j = json.loads(body)
    assert j["ok"] is True and j["source"] == "tone" and 100 <= j["ms"] <= 120 and j["in_rate"] == 8000
    status, _, body = sim.get("/speak")
    assert status == 400 and body.startswith(b"POST /speak (raw s16le mono 8 kHz or WAV)")
    pcm = struct.pack("<%dh" % 1600, *([0] * 1600))         # 1600 samples: 200 ms of raw s16le 8 kHz silence
    status, _, body = sim.get("/speak", method="POST", body=pcm, headers={"Content-Length": str(len(pcm))})
    j = json.loads(body)
    assert status == 200 and j["source"] == "post" and j["in_bytes"] == 3200 and 190 <= j["ms"] <= 200
    wav = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + 3200, b"WAVE", b"fmt ", 16, 1, 1, 16000, 32000, 2, 16, b"data", 3200) + pcm
    status, _, body = sim.get("/speak", method="POST", body=wav)  # the same 1600 samples at 16 kHz: 100 ms
    j = json.loads(body)
    assert status == 200 and j["in_rate"] == 16000 and 90 <= j["ms"] <= 100
    bad = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + 16, b"WAVE", b"fmt ", 16, 1, 1, 8000, 8000, 1, 8, b"data", 16) + b"\x80" * 16
    status, _, body = sim.get("/speak", method="POST", body=bad)
    assert status == 415 and json.loads(body)["hint"].startswith("need PCM 16-bit WAV")
    status, headers, body = sim.get("/mic.wav?seconds=1")
    assert status == 200 and headers["Content-Type"] == "audio/wav" and len(body) == 44 + 32000 == int(headers["Content-Length"])
    assert body[:4] == b"RIFF" and struct.unpack("<I", body[24:28])[0] == 16000
    status, headers, body = sim.get("/talk")
    assert status == 200 and headers["Content-Type"] == "text/html" and b"/speak" in body
    assert "speak: 4 calls" in sim.cmd("talk status") and "mic: 1 calls, 32000 bytes out" in sim.cmd("talk status")


def test_mjpeg_503_in_h264_mode(sim):
    assert sim.cmd("cam mode h264") == "ok\n[cam] mode h264 (applies at the next open/switch)"
    sim.cmd("cam open h264")
    status, _, body = sim.get("/stream")
    assert status == 503 and body == b"camera busy (H.264/RTSP)"
    status, _, body = sim.get("/snapshot.jpg")        # the last JPEG before the switch is still served
    assert status == 200 and body[:2] == b"\xff\xd8"
    sim.cmd("cam mode auto")
    status, _, _ = sim.get("/snapshot.jpg")           # auto: an HTTP request switches back to JPEG
    assert status == 200 and "open 1 jpeg" in sim.cmd("cam stat")
