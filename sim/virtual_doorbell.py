#!/usr/bin/env python3
"""Virtual doorbell: a network stand-in for doorbell-bk7258-fw without the hardware.

It speaks the firmware's three LAN interfaces so `tools/dbcmd.py`, curl and a Home Assistant
generic camera + webhook automation work against it unchanged:

  * TCP command channel (port 2323): one line `<token> <command>`, reply `ok` / `unknown` /
    `denied` followed by the command's REPLY() text, then the connection closes.
  * HTTP (port 8080): `/` status page, `/snapshot.jpg`, `/stream` (MJPEG), the feat/talk routes
    `/speak`, `/mic.wav`, `/talk`, and the simulator's own `/sim/<command>` controls.
  * Webhook reports: the same JSON as `report_ex()` in app_main.c, POSTed to the KV hook on
    boot, ring, motion, wake, test and every 5 min (heartbeat).

Everything that the real board reads from the world (button, PIR, light sensor, battery, USB)
is driven with `sim ...` commands over the same TCP channel (`python3 tools/dbcmd.py sim press`),
over HTTP (`curl http://HOST:8080/sim/press`) or on the simulator's stdin. The always-on
control port (default 2324) keeps accepting `sim` commands while the "device" is asleep or
rebooting and its real ports are down.

Standard library only; Pillow is optional (nicer frames). Python 3.11+.

  python3 sim/virtual_doorbell.py                      # token "sim", ports 2323/8080
  DOORBELL_HOST=127.0.0.1 DOORBELL_TOKEN=sim python3 tools/dbcmd.py sys info
"""
from __future__ import annotations

import argparse
import datetime as _dt
import io
import json
import logging
import math
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable, Optional

try:  # optional: nicer frames
    from PIL import Image, ImageDraw, ImageFont  # type: ignore

    HAVE_PIL = True
except Exception:  # pragma: no cover - depends on the host
    HAVE_PIL = False

log = logging.getLogger("vdb")


class RamLog(logging.Handler):
    """The firmware's 8 KB RAM log ring (policy.c): every line this "firmware" prints."""

    def __init__(self) -> None:
        super().__init__()
        self.buf = bytearray()
        self.total = 0
        self.lock_ = threading.Lock()

    def emit(self, record: logging.LogRecord) -> None:
        data = (record.getMessage() + "\r\n").encode(errors="replace")
        with self.lock_:
            self.buf += data
            self.total += len(data)
            if len(self.buf) > APPLOG_SIZE:
                del self.buf[:len(self.buf) - APPLOG_SIZE]

    def tail(self, n: int) -> bytes:
        with self.lock_:
            return bytes(self.buf[-n:]) if n > 0 else b""

    def all(self) -> bytes:
        with self.lock_:
            return bytes(self.buf)


ramlog = RamLog()
log.addHandler(ramlog)

# ---------------------------------------------------------------- constants mirrored from the firmware
FW_NAME = "doorbell-bk7258-fw"
TCP_CMD_PORT = 2323
CAM_HTTP_PORT = 8080
RTSP_DEFAULT_PORT = 554          # the firmware's default; the simulator defaults to 8554 (no root)
HEARTBEAT_MS = 5 * 60 * 1000
RING_DEBOUNCE_MS = 800
RING_REPORT_MS = 10000
WHITE_LIGHT_MS = 30000
REPLY_MAX = 639                  # s_reply[640]
TCP_LINE_MAX = 191               # char line[192]
HOOK_DEFAULT = ""   # like the firmware (v0.24.1+): no webhook target until `hook set HOST:PORT/PATH`

PIR_SENS_MAX = 3
PIR_CD_MIN_MS = 500
PIR_CD_MAX_MS = 600000
PIR_WARMUP_MS = 5000

NIGHT_SAMPLE_MS = 20000
NIGHT_MOTION_MS = 20000
NIGHT_MIN_ON_MS = 60000
NIGHT_HYST_DEFAULT = 400
NIGHT_SRC_CAM, NIGHT_SRC_MOTION, NIGHT_SRC_FX = 0x01, 0x02, 0x04
PIN_LED_WHITE, PIN_LED_RING = 44, 45

POWER_BOOT_HOLD_MS = 120 * 1000
POWER_SLEEPNOW_GRACE_MS = 10 * 1000
POWER_KEEPALIVE_MIN = 360
POWER_IDLE_MIN_S = 20
POWER_SNAP_MAX_MIN = 1440

RTSP_MAX_CLIENTS = 3
TALK_MAX_LIMIT_S = 120
TALK_MAX_DEFAULT_S = 30
TALK_MIC_MAX_S = 600
TALK_MIC_RATE = 16000            # the ADC runs at 2x the configured 8 kHz
VOC_RATE = 8000                  # voc engine (aud_intf voice mode): 8 kHz G.711 A-law, RTP PT 8
SPK_RATE = 8000
SPK_GAIN_DEFAULT = 0x1C          # VOL_GAIN_MAX: `vol 100`
VOL_GAIN_MAX = 0x1C
POLICY_PATH = "/local/doorbell/policy.json"
POLICY_TIMEOUT_S = 3
APPLOG_SIZE = 8192
LOG_REPLY_MAX = 600
POWER_LOW_BATT_PCT, POWER_LOW_BATT_MIN, POWER_DERATE_PCT = 15, 180, 30
BELL_MS = (66450 - 16) * 1000 // (SPK_RATE * 2)   # the stock bell chime, s16le 8 kHz

# simulator timing (simulated seconds, scaled by `sim time`)
BOOT_IP_S = 1.0                  # Wi-Fi + IP after a boot (measured ~1-2 s)
REBOOT_DOWN_S = 6.0              # `reboot`: boot blink seen ~5.8 s after the reset
OTA_DOWN_S = 35.0                # `ota`: ~35 s end to end on the LAN
CAM_FIRST_FRAME_S = 1.0

RESET_REASONS = {               # bk_idk include/components/system.h
    "power-on": 0x0, "reboot": 0x1, "watchdog": 0x2, "deepsleep-gpio": 0x3, "deepsleep-rtc": 0x4,
    "super-deep": 0xF, "nmi-wdt": 0x10, "ota": 0x18, "unknown": 0xFF,
}


def fw_version_from_source(default: str = "0.0.0-sim") -> str:
    """FW_VERSION from project/main/app_main.c when the simulator runs inside the repo."""
    src = Path(__file__).resolve().parent.parent / "project" / "main" / "app_main.c"
    try:
        m = re.search(r'#define FW_VERSION "([^"]+)"', src.read_text(errors="replace"))
        return m.group(1) if m else default
    except OSError:
        return default


# ---------------------------------------------------------------- simulated clock
class SimClock:
    """Monotonic simulated time; `sim time SPEED` makes it run faster than the wall clock."""

    def __init__(self, speed: float = 1.0) -> None:
        self._lock = threading.Lock()
        self._speed = max(0.01, speed)
        self._real0 = time.monotonic()
        self._sim0 = 0.0

    @property
    def speed(self) -> float:
        return self._speed

    def now_s(self) -> float:
        with self._lock:
            return self._sim0 + (time.monotonic() - self._real0) * self._speed

    def now_ms(self) -> int:
        return int(self.now_s() * 1000)

    def set_speed(self, speed: float) -> None:
        with self._lock:
            now = self._sim0 + (time.monotonic() - self._real0) * self._speed
            self._sim0, self._real0, self._speed = now, time.monotonic(), max(0.01, speed)

    def sleep(self, sim_s: float, stop: Optional[threading.Event] = None, tick: float = 0.05) -> bool:
        """Sleep sim_s simulated seconds. False if `stop` was set first."""
        deadline = self.now_s() + sim_s
        while True:
            left = deadline - self.now_s()
            if left <= 0:
                return True
            if stop is not None and stop.is_set():
                return False
            time.sleep(min(tick, left / self.speed))


# ---------------------------------------------------------------- persistent state (the firmware's KV flash)
class Store:
    def __init__(self, path: Path) -> None:
        self.path = path
        self._lock = threading.Lock()
        self.data: dict = {"kv": {}, "env": {}, "fw": None}
        try:
            loaded = json.loads(path.read_text())
            if isinstance(loaded, dict):
                self.data.update(loaded)
        except (OSError, ValueError):
            pass

    def kv_get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        v = self.data["kv"].get(key)
        return default if v is None else str(v)

    def kv_set(self, key: str, value: str) -> None:
        with self._lock:
            self.data["kv"][key] = str(value)
        self.save()

    def set(self, section: str, key: str, value) -> None:
        with self._lock:
            if section == "fw":
                self.data["fw"] = value
            else:
                self.data.setdefault(section, {})[key] = value
        self.save()

    def save(self) -> None:
        with self._lock:
            tmp = self.path.with_suffix(self.path.suffix + ".tmp")
            try:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                tmp.write_text(json.dumps(self.data, indent=1, sort_keys=True))
                os.replace(tmp, self.path)
            except OSError as e:  # pragma: no cover
                log.warning("[kv] cannot write %s: %s", self.path, e)


# ---------------------------------------------------------------- JPEG frames
_FONT5X7 = {  # 5 columns x 7 rows, '#' = lit; enough for the fallback renderer's overlay text
    "A": "01110 10001 10001 11111 10001 10001 10001", "B": "11110 10001 11110 10001 10001 10001 11110",
    "C": "01110 10001 10000 10000 10000 10001 01110", "D": "11110 10001 10001 10001 10001 10001 11110",
    "E": "11111 10000 11110 10000 10000 10000 11111", "F": "11111 10000 11110 10000 10000 10000 10000",
    "G": "01110 10001 10000 10111 10001 10001 01111", "H": "10001 10001 11111 10001 10001 10001 10001",
    "I": "01110 00100 00100 00100 00100 00100 01110", "J": "00111 00010 00010 00010 10010 10010 01100",
    "K": "10001 10010 10100 11000 10100 10010 10001", "L": "10000 10000 10000 10000 10000 10000 11111",
    "M": "10001 11011 10101 10101 10001 10001 10001", "N": "10001 11001 10101 10011 10001 10001 10001",
    "O": "01110 10001 10001 10001 10001 10001 01110", "P": "11110 10001 10001 11110 10000 10000 10000",
    "Q": "01110 10001 10001 10001 10101 10010 01101", "R": "11110 10001 10001 11110 10100 10010 10001",
    "S": "01111 10000 10000 01110 00001 00001 11110", "T": "11111 00100 00100 00100 00100 00100 00100",
    "U": "10001 10001 10001 10001 10001 10001 01110", "V": "10001 10001 10001 10001 10001 01010 00100",
    "W": "10001 10001 10001 10101 10101 11011 10001", "X": "10001 01010 00100 00100 00100 01010 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100", "Z": "11111 00001 00010 00100 01000 10000 11111",
    "0": "01110 10001 10011 10101 11001 10001 01110", "1": "00100 01100 00100 00100 00100 00100 01110",
    "2": "01110 10001 00001 00110 01000 10000 11111", "3": "11110 00001 00001 01110 00001 00001 11110",
    "4": "00010 00110 01010 10010 11111 00010 00010", "5": "11111 10000 11110 00001 00001 10001 01110",
    "6": "00110 01000 10000 11110 10001 10001 01110", "7": "11111 00001 00010 00100 01000 01000 01000",
    "8": "01110 10001 10001 01110 10001 10001 01110", "9": "01110 10001 10001 01111 00001 00010 01100",
    ":": "00000 00100 00000 00000 00000 00100 00000", ".": "00000 00000 00000 00000 00000 01100 01100",
    "-": "00000 00000 00000 11111 00000 00000 00000", "/": "00001 00010 00010 00100 01000 01000 10000",
    "%": "11001 11010 00010 00100 01000 01011 10011", " ": "00000 00000 00000 00000 00000 00000 00000",
}

_DC_LUM_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]   # JPEG Annex K.3
_DC_LUM_VALS = list(range(12))
_AC_EOB_BITS = [1] + [0] * 15                                     # one 1-bit code: EOB = "0"
_AC_EOB_VALS = [0x00]


def _huff_codes(bits: list[int], vals: list[int]) -> dict[int, tuple[int, int]]:
    codes, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[vals[k]] = (code, length)
            k += 1
            code += 1
        code <<= 1
    return codes


class _BitWriter:
    def __init__(self) -> None:
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, code: int, length: int) -> None:
        self.acc = (self.acc << length) | (code & ((1 << length) - 1))
        self.n += length
        while self.n >= 8:
            b = (self.acc >> (self.n - 8)) & 0xFF
            self.out.append(b)
            if b == 0xFF:
                self.out.append(0x00)
            self.n -= 8
        self.acc &= (1 << self.n) - 1 if self.n else 0

    def flush(self) -> bytes:
        if self.n:
            self.put(0x7F, 8 - self.n)   # pad with ones
        return bytes(self.out)


class MosaicJPEG:
    """Pure-stdlib baseline JPEG: a greyscale image made of flat 8x8 blocks (DC-only), so a
    1280x720 frame is 160x90 "cells". Fast enough for a few frames per second without Pillow."""

    def __init__(self, width: int = 1280, height: int = 720) -> None:
        self.w, self.h = width, height
        self.cw, self.ch = width // 8, height // 8
        self.dc = _huff_codes(_DC_LUM_BITS, _DC_LUM_VALS)
        self.eob = _huff_codes(_AC_EOB_BITS, _AC_EOB_VALS)[0]
        self.header = self._header()

    def _header(self) -> bytes:
        def seg(marker: int, payload: bytes) -> bytes:
            return struct.pack(">HH", marker, len(payload) + 2) + payload

        dqt = seg(0xFFDB, bytes([0]) + bytes([8] + [16] * 63))      # table 0: DC step 8 -> DC coefficient = pixel - 128
        sof = seg(0xFFC0, struct.pack(">BHHB", 8, self.h, self.w, 1) + bytes([1, 0x11, 0]))
        dht = seg(0xFFC4, bytes([0x00]) + bytes(_DC_LUM_BITS) + bytes(_DC_LUM_VALS)
                  + bytes([0x10]) + bytes(_AC_EOB_BITS) + bytes(_AC_EOB_VALS))
        sos = seg(0xFFDA, bytes([1, 1, 0x00, 0, 63, 0]))
        return b"\xff\xd8" + dqt + sof + dht + sos

    def encode(self, cells: list[list[int]]) -> bytes:
        bw, prev, dc = _BitWriter(), 0, self.dc
        for row in cells:
            for v in row:
                coef = max(0, min(255, v)) - 128
                diff = coef - prev
                prev = coef
                if diff == 0:
                    bw.put(*dc[0])
                else:
                    mag = abs(diff)
                    cat = mag.bit_length()
                    bw.put(*dc[cat])
                    bw.put(diff if diff > 0 else diff + (1 << cat) - 1, cat)
                bw.put(*self.eob)
        return self.header + bw.flush() + b"\xff\xd9"

    def text(self, cells: list[list[int]], x: int, y: int, s: str, value: int) -> None:
        for ch in s.upper():
            glyph = _FONT5X7.get(ch, _FONT5X7[" "]).split()
            for r, line in enumerate(glyph):
                for c, bit in enumerate(line):
                    if bit == "1" and 0 <= y + r < self.ch and 0 <= x + c < self.cw:
                        cells[y + r][x + c] = value
            x += 6


class FrameRenderer:
    """Synthetic 1280x720 camera frames: timestamp, "VIRTUAL DOORBELL", light mV, frame counter
    and a moving element so MJPEG streams visibly change. Pillow if available, mosaic otherwise."""

    def __init__(self, width: int = 1280, height: int = 720, quality: int = 80) -> None:
        self.w, self.h, self.q = width, height, quality
        self.mosaic = MosaicJPEG(width, height)
        self._font = None
        if HAVE_PIL:
            try:
                self._font = ImageFont.load_default(size=40)
                self._font_small = ImageFont.load_default(size=26)
            except TypeError:  # Pillow < 10.1
                self._font = self._font_small = ImageFont.load_default()

    def render(self, lines: list[str], n: int, dark: bool) -> bytes:
        return self._render_pil(lines, n, dark) if HAVE_PIL else self._render_mosaic(lines, n, dark)

    def _render_pil(self, lines: list[str], n: int, dark: bool) -> bytes:
        bg = (26, 28, 34) if dark else (96, 128, 150)
        fg = (210, 210, 210) if dark else (255, 255, 255)
        img = Image.new("RGB", (self.w, self.h), bg)
        d = ImageDraw.Draw(img)
        # porch: a door with a frame, the mat and a "visitor" sliding along the bottom
        d.rectangle((self.w * 0.38, self.h * 0.18, self.w * 0.62, self.h * 0.92), fill=(70, 50, 36) if dark else (120, 80, 50),
                    outline=fg, width=4)
        d.ellipse((self.w * 0.585, self.h * 0.55, self.w * 0.60, self.h * 0.58), fill=(220, 190, 90))
        x = int((n * 23) % (self.w + 160)) - 80
        d.ellipse((x - 40, self.h * 0.70, x + 40, self.h * 0.70 + 80), fill=(230, 80, 60))
        d.rectangle((x - 40, self.h * 0.70 + 80, x + 40, self.h * 0.98), fill=(230, 80, 60))
        y = 24
        d.text((28, y), lines[0], fill=fg, font=self._font)
        for s in lines[1:]:
            y += 46
            d.text((28, y), s, fill=fg, font=self._font_small)
        if dark:
            d.text((28, self.h - 50), "night (white LED would be on)", fill=(160, 160, 160), font=self._font_small)
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=self.q)
        return buf.getvalue()

    def _render_mosaic(self, lines: list[str], n: int, dark: bool) -> bytes:
        m = self.mosaic
        bg, fg = (40, 200) if dark else (170, 255)
        cells = [[bg] * m.cw for _ in range(m.ch)]
        for y in range(int(m.ch * 0.18), int(m.ch * 0.92)):       # the door
            for x in range(int(m.cw * 0.38), int(m.cw * 0.62)):
                cells[y][x] = 80 if dark else 110
        x = (n * 3) % (m.cw + 12) - 6                            # the visitor
        for y in range(int(m.ch * 0.72), m.ch):
            for xx in range(x - 3, x + 4):
                if 0 <= xx < m.cw:
                    cells[y][xx] = 20 if not dark else 230
        y = 2
        for s in lines:
            m.text(cells, 2, y, s[:26], fg)
            y += 9
        return m.encode(cells)


# ---------------------------------------------------------------- reply buffer
class Reply:
    """What REPLY() appends for the TCP client; UART output (os_printf) only reaches the log."""

    def __init__(self) -> None:
        self.text = ""

    def add(self, s: str) -> None:
        for line in s.rstrip("\r\n").split("\r\n"):
            log.info("%s", line)
        self.text = (self.text + s)[:REPLY_MAX]


# ---------------------------------------------------------------- the device
@dataclass
class CamState:
    open: bool = False
    ever_opened: bool = False
    h264: bool = False
    pref: int = 0                      # 0 auto, 1 jpeg, 2 h264
    w: int = 0
    h: int = 0
    frames: int = 0
    h264_frames: int = 0
    drop: int = 0
    seq: int = 0
    last_ms: int = 0
    opened_ms: int = 0
    jpeg: bytes = b""


@dataclass
class TalkState:
    speaking: bool = False
    listening: bool = False
    abort: bool = False
    speak_calls: int = 0
    speak_last_ms: int = 0
    speak_last_rate: int = 0
    speak_in_bytes: int = 0
    mic_calls: int = 0
    mic_out_bytes: int = 0
    last_err: int = 0


@dataclass
class SdState:
    present: bool = True
    mounted: bool = True
    cap_mb: int = 7580
    free_mb: int = 7412
    saved: int = 0
    fail: int = 0
    drop: int = 0
    last_file: str = ""
    files: list = field(default_factory=list)
    fmt_token: int = 0
    fmt_token_ms: int = 0
    result: str = "mounted"


class Device:
    """State machine + command set of the firmware, minus the silicon."""

    def __init__(self, args: argparse.Namespace, store: Store, clock: SimClock) -> None:
        self.args, self.store, self.clock = args, store, clock
        self.lock = threading.RLock()
        self.stop_all = threading.Event()
        self.fw = args.fw_version or store.data.get("fw") or fw_version_from_source()
        store.set("fw", "", self.fw)
        env = store.data.get("env", {})
        self.light_mv = int(env.get("light_mv", 740))
        self.batt_mv = int(env.get("batt_mv", 3886))
        self.usb = int(env.get("usb", 1))
        self.charging = int(env.get("charging", 0))
        self.token = args.token
        self.hook = store.kv_get("db_hook", args.hook or HOOK_DEFAULT) if not args.hook else args.hook
        self.hook_unset_logged = False   # "hook not set" is logged once per boot, like report_ex()
        if args.hook:
            store.kv_set("db_hook", args.hook)
        self.ssid = store.kv_get("db_ssid", "virtual-lan") or ""
        self.ip = args.ip
        self.renderer = FrameRenderer(quality=80)
        self.cam = CamState()
        self.talk = TalkState()
        self.sd = SdState()
        self.rtsp = RtspStandIn(self)
        self.spk_gain_raw = -1               # `spk cfg gain N` raw override; -1 = follow `vol`
        self.policy_result, self.policy_rev, self.policy_detail, self.policy_at_ms = "none", "", "not fetched", 0
        self.policy_fetched = False
        self.low_batt = False
        self.spk_busy = False
        self.mic_busy = False
        self.pa_pin = 46
        self.ota_running = False
        self.http_clients = 0
        self._http_sockets: set[socket.socket] = set()
        self.net = Network(self)
        self.reports: list[dict] = []        # last reports (for `sim status` and tests through /sim/reports)
        self._load_settings()
        self._reset_runtime("power-on", None)

    # ------------------------------------------------------------ settings (KV)
    def _load_settings(self) -> None:
        kv = self.store.kv_get
        self.dark_mv = int(kv("db_dark_mv", "1400"))
        self.fx_enabled = kv("db_fx", "1") != "0"
        self.light_chan = int(kv("db_light_chan", "14"))
        self.pir_on = kv("db_pir_on", "1") != "0"
        self.pir_sens = min(PIR_SENS_MAX, int(kv("db_pir_sens", "1")))
        self.pir_cd_ms = int(kv("db_pir_cd", "10000"))
        self.night_auto = kv("db_night_auto", "1") != "0"
        self.night_ring = kv("db_night_ring", "1") != "0"
        self.night_hyst = int(kv("db_night_hyst", str(NIGHT_HYST_DEFAULT)))
        self.pwr_sleep = kv("db_pwr_sleep", "0") == "1"
        self.pwr_force = kv("db_pwr_force", "0") == "1"
        self.pwr_idle_s = max(POWER_IDLE_MIN_S, int(kv("db_pwr_idle", "60")))
        self.pwr_wake_pir = kv("db_pwr_wpir", "0") == "1"
        self.pwr_snap_min = int(kv("db_pwr_snap", "0"))
        self.pwr_bchan = int(kv("db_pwr_bchan", "15"))
        self.pwr_bratio = int(kv("db_pwr_bratio", "200"))
        self.sched_on, self.day_start, self.day_end, self.day_min, self.night_min = True, 7 * 60, 22 * 60, 15, 60
        self.sched_parse(kv("db_pwr_sched", "07:00 22:00 15 60") or "07:00 22:00 15 60")
        self.policy_cfg = kv("db_pwr_policy", "auto") or "auto"
        self.vol = min(100, int(kv("db_vol", "100")))
        mode = kv("db_cam_mode", "auto")
        self.cam.pref = {"jpeg": 1, "h264": 2}.get(mode or "auto", 0)
        q = (kv("db_h264_q", "120 24") or "120 24").split()
        self.h264_imb, self.h264_pmb = (int(q[0]), int(q[1])) if len(q) == 2 else (120, 24)
        self.rtsp_on = kv("db_rtsp", "0") == "1"
        self.rtsp_port = int(kv("db_rtsp_port", str(self.args.rtsp_port)))
        self.talk_gain = int(kv("db_talk_gain", "0"))
        self.talk_micgain = int(kv("db_talk_micgain", "63"))
        self.talk_max_s = int(kv("db_talk_max", str(TALK_MAX_DEFAULT_S)))
        self.talk_aec = kv("db_talk_aec", "1") == "1"          # feat/duplex: voc engine AEC (audio_voc.c)
        self.voc_mic = int(kv("db_voc_mic", "0"))               # 0 = default 0x3f
        self.voc_on = False                                     # voc engine running (rtsp audio client or `talk voc on`)
        self.voc_manual = False
        self.voc_starts = 0
        self.sd_auto = kv("db_sd_auto", "1") != "0"
        self.sd_keep_s = max(5, int(kv("db_sd_keep", "30")))
        self.hook = kv("db_hook", self.hook) or HOOK_DEFAULT
        stored_token = kv("db_token")
        if stored_token is not None and not self.args.token_from_cli:
            self.token = stored_token

    def _reset_runtime(self, boot_reason: str, wake_reason: Optional[str]) -> None:
        now = self.clock.now_ms()
        self.boot_ms = now
        self.boot_reason, self.wake_reason = boot_reason, wake_reason or ""
        self.got_ip = False
        self.state = "booting"
        self.ring_count = self.motion_count = 0
        self.last_ring_ms = self.last_ring_report_ms = -10 ** 9
        self.last_motion_ms = 0
        self.pend_boot = self.pend_ring = self.pend_motion = self.pend_test = self.pend_fx = False
        self.wake_pending = bool(wake_reason)
        self.last_hb_ms = now
        self.fx_running = False
        self.pir_count = self.pir_edges = self.pir_ignored = self.pir_missed = 0
        self.pir_last_ms = 0
        self.pir_armed_ms = now
        self.white_mask, self.white_manual, self.white_out = 0, -1, False
        self.ring_manual, self.ring_out = -1, False
        self.cam_dark, self.night_samples, self.night_last_mv = False, 0, 0
        self.night_motion_until = 0
        self.night_cam_on_ms = self.night_last_sample_ms = 0
        self.last_activity_ms = now
        self.sleep_at_ms = 0
        self.sleeping = False
        self.cam = CamState(pref=self.cam.pref)
        self.talk = TalkState()
        self.spk_busy = self.mic_busy = False
        self.ota_running = False
        self.policy_fetched = False
        self.policy_result, self.policy_rev, self.policy_detail, self.policy_at_ms = "none", "", "not fetched", 0

    # ------------------------------------------------------------ helpers
    def spk_gain(self) -> int:
        """spk_gain_now(): the raw override or the volume mapping (policy.c vol_gain)."""
        return self.spk_gain_raw if self.spk_gain_raw >= 0 else (self.vol * VOL_GAIN_MAX + 50) // 100

    # ---- smart RTC schedule (power.c, fw 0.25)
    @staticmethod
    def _hhmm(s: str) -> Optional[int]:
        m = re.fullmatch(r"(\d{1,2}):(\d{2})", s)
        if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
            return None
        return int(m.group(1)) * 60 + int(m.group(2))

    def sched_parse(self, text: str) -> bool:
        if text == "off":
            self.sched_on = False
            return True
        tok = text.split()
        if len(tok) != 4:
            return False
        st, en = self._hhmm(tok[0]), self._hhmm(tok[1])
        if st is None or en is None or not tok[2].isdigit() or not tok[3].isdigit():
            return False
        dm, nm = int(tok[2]), int(tok[3])
        if not (1 <= dm <= POWER_SNAP_MAX_MIN and 1 <= nm <= POWER_SNAP_MAX_MIN):
            return False
        self.sched_on, self.day_start, self.day_end, self.day_min, self.night_min = True, st, en, dm, nm
        return True

    def sched_str(self) -> str:
        if not self.sched_on:
            return "off (fixed snap_min %u)" % self.pwr_snap_min
        return "%02d:%02d-%02d:%02d day %u night %u" % (self.day_start // 60, self.day_start % 60, self.day_end // 60,
                                                        self.day_end % 60, self.day_min, self.night_min)

    def sched_is_day(self, t: int) -> bool:
        if self.day_start == self.day_end:
            return True
        if self.day_start < self.day_end:
            return self.day_start <= t < self.day_end
        return t >= self.day_start or t < self.day_end

    def wake_interval(self) -> tuple[int, str]:
        """power_wake_interval(): minutes until the next RTC wake and why; refreshes the low-battery flag."""
        pct = self.batt_pct(self.power_batt_mv(), bool(self.charging))
        if not self.sched_on:
            mins = self.pwr_snap_min or POWER_KEEPALIVE_MIN
            why = "fixed snap_min%s" % ("" if self.pwr_snap_min else " (keepalive)")
        else:
            now = _dt.datetime.now()      # the simulator's clock is always "synced"
            day = self.sched_is_day(now.hour * 60 + now.minute)
            mins = self.day_min if day else self.night_min
            why = "%s by clock %02d:%02d" % ("day" if day else "night", now.hour, now.minute)
        self.low_batt = pct < POWER_LOW_BATT_PCT
        if self.low_batt:
            mins = max(mins, POWER_LOW_BATT_MIN)
            why += ", batt %u%%<%u: %u min, pir wake off" % (pct, POWER_LOW_BATT_PCT, POWER_LOW_BATT_MIN)
        elif pct < POWER_DERATE_PCT:
            mins *= 2
            why += ", batt %u%%<%u: x2" % (pct, POWER_DERATE_PCT)
        return max(1, min(POWER_SNAP_MAX_MIN, mins)), why

    # ---- policy pull (policy.c)
    def policy_url(self) -> Optional[str]:
        if self.policy_cfg == "off":
            return None
        if self.policy_cfg == "auto":
            m = re.match(r"^([^/:]+):(\d+)", self.hook or "")
            return "http://%s:%s%s" % (m.group(1), m.group(2), POLICY_PATH) if m else None
        return self.policy_cfg

    def policy_fetch(self, nested: bool) -> None:
        self.policy_at_ms, self.policy_rev = self.clock.now_ms(), ""
        url = self.policy_url()
        if not url:
            self.policy_result, self.policy_detail = "none", "off" if self.policy_cfg == "off" else "no url (hook not set)"
            log.info("[policy] %s", self.policy_detail)
            return
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "doorbell-policy"}), timeout=POLICY_TIMEOUT_S) as r:
                body = r.read(1024).decode(errors="replace")
            doc = json.loads(body)
            if not isinstance(doc, dict):
                raise ValueError("not an object")
        except urllib.error.HTTPError as e:
            self.policy_result, self.policy_detail = "error", "HTTP %d" % e.code
            log.info("[policy] %s: %s", url, self.policy_detail)
            return
        except (urllib.error.URLError, OSError, ValueError) as e:
            self.policy_result, self.policy_detail = "error", "connect failed (%s)" % (getattr(e, "reason", None) or e)
            log.info("[policy] %s: %s", url, self.policy_detail)
            return
        self.policy_rev = str(doc.get("rev", ""))[:23]
        changed, same, bad, what = 0, 0, 0, []

        def run(cmd: str, key: str, val) -> None:
            nonlocal changed
            self.run_line(cmd)
            changed += 1
            what.append("%s=%s" % (key, val))

        def uint(v, lo, hi) -> Optional[int]:
            try:
                n = int(float(v))
            except (TypeError, ValueError):
                return None
            return n if lo <= n <= hi else None

        def flag(v) -> Optional[bool]:
            return {"1": True, "true": True, "on": True, "0": False, "false": False, "off": False}.get(str(v).lower())

        for key, lo, hi, cur, fmt in (("volume", 0, 100, lambda: self.vol, "vol %u"), ("pir_sens", 0, 2, lambda: self.pir_sens, "pir sens %u"),
                                      ("dark_mv", 100, 4000, lambda: self.dark_mv, "ring dark %u"), ("keep_s", 5, 3600, lambda: self.sd_keep_s, "sd keep %u")):
            if key in doc:
                n = uint(doc[key], lo, hi)
                if n is None:
                    bad += 1
                elif n != cur():
                    run(fmt % n, key, doc[key])
                else:
                    same += 1
        for key, cur, fmt in (("wake_pir", lambda: self.pwr_wake_pir, "pwr wake_pir %s"), ("night_auto", lambda: self.night_auto, "night auto %s"),
                              ("sd_auto", lambda: self.sd_auto, "sd auto %s")):
            if key in doc:
                b = flag(doc[key])
                if b is None:
                    bad += 1
                elif b != cur():
                    run(fmt % ("on" if b else "off"), key, doc[key])
                else:
                    same += 1
        st, en, dm, nm, diff, any_ = self.day_start, self.day_end, self.day_min, self.night_min, False, False
        for key in ("day_start", "day_end"):
            if key in doc:
                any_ = True
                t = self._hhmm(str(doc[key]))
                if t is None:
                    bad += 1
                elif key == "day_start" and t != st:
                    st, diff = t, True
                elif key == "day_end" and t != en:
                    en, diff = t, True
        for key in ("day_min", "night_min"):
            if key in doc:
                any_ = True
                n = uint(doc[key], 1, POWER_SNAP_MAX_MIN)
                if n is None:
                    bad += 1
                elif key == "day_min" and n != dm:
                    dm, diff = n, True
                elif key == "night_min" and n != nm:
                    nm, diff = n, True
        if any_ and not self.sched_on:
            what.append("sched=skipped(off)")
        elif diff:
            run("pwr sched %02d:%02d %02d:%02d %u %u" % (st // 60, st % 60, en // 60, en % 60, dm, nm), "sched",
                "%02d:%02d-%02d:%02d/%u/%u" % (st // 60, st % 60, en // 60, en % 60, dm, nm))
        elif any_:
            same += 1
        self.policy_result = "applied"
        self.policy_detail = "%d changed %d same%s" % (changed, same, " BAD values" if bad else "")
        log.info("[policy] rev %s: %s%s", self.policy_rev or "-", self.policy_detail, (" | " + " ".join(what)) if what else "")

    def uptime_ms(self) -> int:
        return self.clock.now_ms() - self.boot_ms

    def activity(self) -> None:
        self.last_activity_ms = self.clock.now_ms()

    def adc_mv(self, chan: int) -> int:
        j = random.randint(-3, 3)
        if chan == 9:
            return 3068 + j
        if chan == self.light_chan:
            return max(0, self.light_mv + j)
        if chan == self.pwr_bchan:
            return max(0, self.batt_mv * 100 // self.pwr_bratio + j)
        if chan == 10:
            return 300 + j
        return 24 + abs(j)

    def light_read_mv(self) -> int:
        return self.adc_mv(self.light_chan)

    def power_batt_mv(self) -> int:
        return self.adc_mv(self.pwr_bchan) * self.pwr_bratio // 100

    @staticmethod
    def batt_pct(mv: int, chg: bool) -> int:
        tbl = [(4150, 100), (4080, 95), (4000, 88), (3920, 78), (3850, 66), (3790, 55), (3740, 45),
               (3700, 35), (3660, 25), (3620, 15), (3570, 8), (3500, 3), (3400, 0)]
        if chg:
            mv = mv - 80 if mv > 80 else 0
        if mv >= tbl[0][0]:
            return 100
        for i in range(1, len(tbl)):
            if mv >= tbl[i][0]:
                hi_mv, hi_p = tbl[i - 1]
                lo_mv, lo_p = tbl[i]
                return lo_p + (hi_p - lo_p) * (mv - lo_mv) // (hi_mv - lo_mv)
        return 0

    def busy(self) -> bool:
        b = self.spk_busy or self.mic_busy or self.fx_running or self.ota_running or self.http_clients > 0
        if self.got_ip and (self.pend_boot or self.pend_ring or self.pend_motion or self.pend_test or self.pend_fx):
            b = True
        return b

    def is_dark(self, mv: Optional[int] = None) -> bool:
        return (self.light_read_mv() if mv is None else mv) > self.dark_mv

    # ------------------------------------------------------------ reports (webhook)
    def report_json(self, event: str, extra: str = "") -> str:
        chg = bool(self.charging)
        bmv = self.power_batt_mv()
        sleep_min, _ = self.wake_interval()
        if event == "boot":   # report_ex("boot", policy_json()): the policy pull result rides on the boot report
            extra = ',"policy":"%s","policy_rev":"%s"' % (self.policy_result, self.policy_rev) + extra
        return ('{"event":"%s","fw":"%s","ip":"%s","mv":%u,"light_mv":%u,"uptime_s":%u,"motion_count":%u,'
                '"ring_count":%u,"pir_on":%u,"pir_sens":%u,"batt_mv":%u,"batt_pct":%u,"usb":%u,"charging":%u,"sleep_min":%u,"wake_pir":%u%s}'
                % (event, self.fw, self.ip, self.adc_mv(9), self.light_read_mv(), self.uptime_ms() // 1000,
                   self.motion_count, self.ring_count, 1 if self.pir_on else 0, self.pir_sens,
                   bmv, self.batt_pct(bmv, chg), self.usb, 1 if chg else 0, sleep_min,
                   1 if self.pwr_wake_pir and not self.low_batt else 0, extra))

    def report(self, event: str, extra: str = "") -> int:
        body = self.report_json(event, extra)
        if not self.hook:   # report_ex(): no target configured -> skip the POST, say so once, -ENOENT
            if not self.hook_unset_logged:
                self.hook_unset_logged = True
                log.info("[hook] %s skipped: hook not set (hook set HOST:PORT/PATH)", event)
            return -2
        status = self.http_post_json(self.hook, body)
        log.info("[hook] %s -> %d", event, status)
        with self.lock:
            self.reports.append({"t": time.time(), "status": status, "json": json.loads(body)})
            del self.reports[:-50]
        return status

    @staticmethod
    def http_post_json(hook: str, body: str) -> int:
        """Like the firmware's http_post_json(): hook = host:port/path, HTTP/1.1 POST, no auth."""
        m = re.match(r"^([^/:]+)(?::(\d+))?(/.*)?$", hook)
        if not m:
            return -22
        host, port, path = m.group(1), int(m.group(2) or 80), m.group(3) or "/"
        req = urllib.request.Request(f"http://{host}:{port}{path}", data=body.encode(), method="POST",
                                     headers={"Content-Type": "application/json", "User-Agent": FW_NAME})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            errno = getattr(getattr(e, "reason", None), "errno", None)
            return -(errno or 111)

    # ------------------------------------------------------------ inputs
    def press(self) -> None:
        """Button GPIO 53 falling edge (button_isr)."""
        now = self.clock.now_ms()
        if now - self.last_ring_ms > RING_DEBOUNCE_MS:
            self.last_ring_ms = now
            self.ring_count += 1
            self.activity()
            self.pend_fx = True
            if now - self.last_ring_report_ms > RING_REPORT_MS:
                self.last_ring_report_ms = now
                self.pend_ring = True

    def pir_trigger(self) -> str:
        """A PIR edge through pir.c's filter: 'off' | 'warmup' | 'cooldown' | 'accepted'."""
        now = self.clock.now_ms()
        if not self.pir_on:
            return "off"
        self.pir_edges += 1
        if now - self.pir_armed_ms < PIR_WARMUP_MS:
            self.pir_ignored += 1
            return "warmup"
        if self.pir_count and now - self.pir_last_ms < self.pir_cd_ms:
            self.pir_ignored += 1
            return "cooldown"
        self.pir_count += 1
        self.pir_last_ms = now
        self.last_motion_ms = now          # motion_cb()
        self.motion_count += 1
        self.pend_motion = True
        self.activity()
        return "accepted"

    def motion_text(self, r: str) -> str:
        now = self.clock.now_ms()
        if r == "accepted":
            return "[motion] simulated edge accepted: count %u, report queued\r\n" % self.pir_count
        if r == "off":
            return "[motion] ignored: pir off (`pir on`)\r\n"
        if r == "warmup":
            return "[motion] ignored: warm-up, %u ms left\r\n" % (PIR_WARMUP_MS - (now - self.pir_armed_ms))
        return "[motion] ignored: cooldown, %u ms left (count %u)\r\n" % (self.pir_cd_ms - (now - self.pir_last_ms), self.pir_count)

    # ------------------------------------------------------------ night light / LEDs (night.c)
    def white_set(self, src: int, on: bool) -> None:
        with self.lock:
            self.white_mask = (self.white_mask | src) if on else (self.white_mask & ~src)
            self._white_apply()

    def _white_apply(self) -> None:
        out = bool(self.white_manual) if self.white_manual >= 0 else bool(self.white_mask)
        if out != self.white_out:
            self.white_out = out
            log.info("[night] white LED (GPIO %d) %s (src 0x%x%s)", PIN_LED_WHITE, "ON" if out else "off", self.white_mask,
                     ", manual" if self.white_manual >= 0 else "")

    def led_manual(self, which: int, state: int) -> None:
        with self.lock:
            if which == 0:
                self.white_manual = state
                self._white_apply()
            else:
                self.ring_manual = state
                out = bool(state) if state >= 0 else False
                if out != self.ring_out:
                    self.ring_out = out
                    log.info("[night] blue ring (GPIO %d) %s (manual)", PIN_LED_RING, "ON" if out else "off")

    def night_on_motion(self) -> None:
        if not self.night_auto:
            return
        mv = self.light_read_mv()
        if mv > self.dark_mv:
            self.night_motion_until = self.clock.now_ms() + NIGHT_MOTION_MS
            self.white_set(NIGHT_SRC_MOTION, True)
            log.info("[night] motion in the dark (%u mV): white light %u s", mv, NIGHT_MOTION_MS // 1000)

    def night_tick(self) -> None:
        now = self.clock.now_ms()
        if self.night_motion_until and now >= self.night_motion_until:
            self.night_motion_until = 0
            self.white_set(NIGHT_SRC_MOTION, False)
        if self.cam.open and self.night_auto:
            if now - self.night_last_sample_ms >= NIGHT_SAMPLE_MS:
                self.night_last_sample_ms = now
                mv = self.light_read_mv()
                self.night_last_mv = mv
                self.night_samples += 1
                if not self.cam_dark and mv > self.dark_mv:
                    self.cam_dark = True
                    self.night_cam_on_ms = now
                    log.info("[night] camera open and dark (%u mV > %u): white light on", mv, self.dark_mv)
                    self.white_set(NIGHT_SRC_CAM, True)
                elif self.cam_dark and mv < self.dark_mv - self.night_hyst and now - self.night_cam_on_ms >= NIGHT_MIN_ON_MS:
                    self.cam_dark = False
                    log.info("[night] light is back (%u mV): camera night light released", mv)
                    self.white_set(NIGHT_SRC_CAM, False)
        elif self.cam_dark:
            self.cam_dark = False
            self.white_set(NIGHT_SRC_CAM, False)

    def ringfx(self) -> None:
        """ringfx_task: bell, 8 blue blinks, white light 30 s when dark."""
        if not self.fx_enabled or self.fx_running:
            return
        self.fx_running = True

        def run() -> None:
            light = self.light_read_mv()
            dark = light > self.dark_mv
            log.info("[fx] ring: light %u mV (%s)", light, "dark" if dark else "day")
            if not self.spk_busy:
                self.spk_play(BELL_MS, "bell")
            if dark:
                self.white_set(NIGHT_SRC_FX, True)
            log.info("[fx] blue ring (GPIO %d): 8 blinks", PIN_LED_RING)
            self.clock.sleep(2.4, self.stop_all)
            if dark:
                self.clock.sleep((WHITE_LIGHT_MS - 2400) / 1000, self.stop_all)
                self.white_set(NIGHT_SRC_FX, False)
            self.fx_running = False

        threading.Thread(target=run, name="ringfx", daemon=True).start()

    # ------------------------------------------------------------ speaker (spk / talk)
    def spk_play(self, ms: int, what: str, hz: int = 0) -> None:
        self.spk_busy = True
        log.info("[spk] %s %u ms (PA GPIO %d on, gain 0x%x)", what, ms, self.pa_pin, self.spk_gain())
        if self.args.audio and hz:
            play_tone_on_host(hz, ms)

        def run() -> None:
            self.clock.sleep(ms / 1000, self.stop_all)
            log.info("[spk] done, %u samples, %u dma frames, 0 underruns", ms * SPK_RATE // 1000, ms * SPK_RATE // 1000 // 320)
            self.spk_busy = False

        threading.Thread(target=run, name="spk", daemon=True).start()

    # ------------------------------------------------------------ camera
    def cam_open(self, w: int, h: int, h264: bool) -> int:
        with self.lock:
            if self.cam.open:
                if self.cam.h264 == h264:
                    return 0
                self.cam_close()
            now = self.clock.now_ms()
            self.cam.open = self.cam.ever_opened = True
            self.cam.h264, self.cam.w, self.cam.h = h264, w, h
            self.cam.frames = self.cam.h264_frames = 0
            self.cam.opened_ms = now
            self.night_last_sample_ms = now
            log.info("[cam] open %ux%u %s: rails 9/28/5/6 high, PWDN pulse, sensor ov9650 (first frame in ~1 s)", w, h,
                     "h264" if h264 else "jpeg")
            self.activity()
            return 0

    def cam_close(self) -> None:
        with self.lock:
            if self.cam.open:
                log.info("[cam] close (rails low)")
            self.cam.open = False
            self.cam.h264 = False

    def cam_want_jpeg(self) -> int:
        """0 = camera is (now) in JPEG mode, -1 = stay / 503 (cam mode policy)."""
        with self.lock:
            if self.cam.open and not self.cam.h264:
                return 0
            if self.cam.pref == 2:
                return -1
            if self.cam.open and self.cam.h264:
                if self.rtsp.playing() > 0:
                    return -1
                self.cam_close()
            return self.cam_open(1280, 720, False)

    def cam_frame_tick(self) -> None:
        """One tick of the frame producer (runs at --fps real frames per second)."""
        if not self.cam.open or (self.clock.now_ms() - self.cam.opened_ms) < CAM_FIRST_FRAME_S * 1000:
            return
        if self.cam.h264:
            self.cam.h264_frames += 3     # 15 fps nominal, ticked at 5 Hz
            return
        n = self.cam.frames + 1
        ts = _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        lines = [ts, "VIRTUAL DOORBELL fw %s" % self.fw, "light %u mV  frame %u" % (self.light_mv, n),
                 "batt %u mV  usb %u  ring %u  motion %u" % (self.batt_mv, self.usb, self.ring_count, self.motion_count)]
        jpeg = self.renderer.render(lines, n, self.is_dark(self.light_mv))
        with self.lock:
            self.cam.jpeg = jpeg
            self.cam.frames = n
            self.cam.seq += 1
            self.cam.last_ms = self.clock.now_ms()

    # ------------------------------------------------------------ SD card stand-in
    def sd_save(self, event: str) -> str:
        if not self.sd.mounted:
            self.sd.fail += 1
            return "not mounted"
        if not self.cam.jpeg:
            self.sd.fail += 1
            return "no frame"
        name = "1:/doorbell/%s_%s.jpg" % (_dt.datetime.now().strftime("%Y%m%d_%H%M%S"), event)
        self.sd.files.append((name, len(self.cam.jpeg)))
        self.sd.saved += 1
        self.sd.last_file = name
        self.sd.free_mb = max(0, self.sd.free_mb - (1 if self.sd.saved % 12 == 0 else 0))
        return "saved %s (%u KB)" % (name, len(self.cam.jpeg) // 1024)

    # ------------------------------------------------------------ power / sleep / reboot
    def power_tick(self) -> None:
        now = self.clock.now_ms()
        if self.sleeping:
            return
        if self.sleep_at_ms and (now - self.sleep_at_ms) >= 0:
            self.sleep_at_ms = 0
            self.enter_sleep("sleepnow")
            return
        if not self.pwr_sleep or self.uptime_ms() < POWER_BOOT_HOLD_MS:
            return
        if self.usb and not self.pwr_force:
            return
        if self.busy() or (now - self.last_activity_ms) // 1000 < self.pwr_idle_s:
            return
        self.enter_sleep("idle %u s" % ((now - self.last_activity_ms) // 1000))

    def enter_sleep(self, why: str) -> None:
        if self.sleeping:
            return
        self.sleeping = True
        self.state = "asleep"
        rtc_min, swhy = self.wake_interval()
        log.info("[pwr] deep sleep (%s): amp off, LEDs off, camera closed, rails low, wifi stop; rtc wake in %u min (%s), "
                 "wake on button%s, usb plug, uart rx", why, rtc_min, swhy, ", pir" if self.pwr_wake_pir and not self.low_batt else "")
        self.cam_close()
        self.white_manual, self.white_mask = -1, 0
        self._white_apply()
        self.wake_event: Optional[str] = None     # set by wake(); initialised before the ports go down
        threading.Thread(target=self._sleep_thread, args=(rtc_min,), name="sleep", daemon=True).start()

    def _sleep_thread(self, rtc_min: int) -> None:
        self.net.down()
        deadline = self.clock.now_s() + rtc_min * 60
        while not self.stop_all.is_set():
            if self.wake_event:
                reason = self.wake_event
                break
            if self.clock.now_s() >= deadline:
                reason = "rtc"
                break
            time.sleep(0.02)
        else:
            return
        self.sleeping = False
        self.boot("deepsleep-rtc" if reason == "rtc" else "deepsleep-gpio", reason)

    def wake(self, reason: str) -> None:
        """A wake source fired while asleep (button | pir | usb)."""
        if self.sleeping:
            self.wake_event = reason

    def reboot(self, reason: str, down_s: float) -> None:
        """Drop everything for down_s simulated seconds, then boot with `reason`."""
        def run() -> None:
            time.sleep(0.15)                 # let the TCP reply out (the firmware waits 50 ms + closes)
            self.state = "rebooting"
            self.cam_close()
            self.net.down()
            log.info("[boot] down for ~%.0f s (%s)", down_s, reason)
            if self.clock.sleep(down_s, self.stop_all):
                self.boot(reason, None)

        threading.Thread(target=run, name="reboot", daemon=True).start()

    def boot(self, reason: str, wake_reason: Optional[str]) -> None:
        with self.lock:
            self._load_settings()
            self._reset_runtime(reason, wake_reason)
            log.info("==== %s %s: hello from our own firmware ==== (reset 0x%x %s%s)", FW_NAME, self.fw,
                     RESET_REASONS.get(reason, 0xFF), reason, " wake=" + wake_reason if wake_reason else "")
            log.info("[night] boot blink (blue ring 150 ms)")
            if wake_reason == "button":      # app_power_wake_input(53): chime at once, report after the IP
                self.ring_count += 1
                self.last_ring_ms = self.last_ring_report_ms = self.clock.now_ms()
                self.pend_fx = self.pend_ring = True
            elif wake_reason == "pir":
                self.motion_count += 1
                self.pend_motion = True
            log.info("[cfg] ssid '%s' hook %s", self.ssid, self.hook or "not set")
        self.net.up()
        log.info("[tcp] command channel on port %d", self.args.tcp_port)
        log.info("[cam] http server on port %d", self.args.http_port)
        if self.rtsp_on:
            self.rtsp.start(self.rtsp_port)

        def got_ip() -> None:
            if self.clock.sleep(BOOT_IP_S, self.stop_all) and self.state == "booting":
                self.got_ip = True
                self.state = "awake"
                log.info("[wifi] got ip %s", self.ip)
                if not self.policy_fetched:      # policy_boot_fetch(): once per boot, before the boot report
                    self.policy_fetched = True
                    self.policy_fetch(False)
                self.pend_boot = True

        threading.Thread(target=got_ip, name="wifi", daemon=True).start()

    # ------------------------------------------------------------ background tasks
    def reporter_loop(self) -> None:
        """reporter_task(): pending flags -> webhook, heartbeat, night/power ticks."""
        last_frame = 0.0
        while not self.stop_all.is_set():
            try:
                if self.state in ("awake", "booting") and not self.sleeping:
                    if self.pend_boot:
                        self.pend_boot = False
                        self.report("boot")
                        self.last_hb_ms = self.clock.now_ms()
                    if self.pend_fx:
                        self.pend_fx = False
                        self.ringfx()
                    if self.got_ip and self.pend_ring:
                        self.pend_ring = False
                        self.report("ring")
                        if self.sd_auto:
                            log.info("[sd] auto-save ring: %s", self.sd_save("ring"))
                    if self.got_ip and self.pend_motion:
                        self.pend_motion = False
                        self.night_on_motion()
                        self.report("motion")
                        if self.sd_auto:
                            log.info("[sd] auto-save motion: %s", self.sd_save("motion"))
                    if self.got_ip and self.wake_pending:
                        self.wake_pending = False
                        self.report("wake", ',"reason":"%s"' % self.wake_reason)
                    if self.pend_test:
                        self.pend_test = False
                        self.report("test")
                    if self.got_ip and self.clock.now_ms() - self.last_hb_ms > HEARTBEAT_MS:
                        self.last_hb_ms = self.clock.now_ms()
                        self.report("heartbeat")
                    self.night_tick()
                    self.power_tick()
                    now = time.monotonic()
                    if now - last_frame >= 1.0 / max(0.5, self.args.fps):
                        last_frame = now
                        self.cam_frame_tick()
            except Exception:  # pragma: no cover - keep the device alive
                log.exception("reporter loop")
            time.sleep(0.02)

    # ------------------------------------------------------------ command channel
    def run_line(self, line: str, allow: Optional[set[str]] = None) -> tuple[str, str]:
        """run_line(): returns ('ok'|'unknown', reply text)."""
        argv = line.split()[:12]
        if not argv:
            return "unknown", ""
        handler = self.COMMANDS.get(argv[0])
        if handler is None or (allow is not None and argv[0] not in allow):
            return "unknown", ""
        r = Reply()
        try:
            handler(self, argv, r)
        except Exception as e:  # pragma: no cover
            log.exception("command %s", line)
            r.add("[sim] command crashed: %s\r\n" % e)
        return "ok", r.text

    # ---- pin / adc / mem / reboot
    def cmd_pin(self, argv: list[str], r: Reply) -> None:
        if len(argv) < 2:
            log.info("pin all | mux | dump | get N | in N [up|down|none] | hiz N | out N 0|1 | raw N HEX | blink N [count] | watch SECONDS")
            return
        if argv[1] == "all":
            bits = "".join("1" if p in (9, 19, 28, 53) else "0" for p in range(56))
            log.info("PINS %s", bits)
        else:
            log.info("[pin] %s (UART-only output on the real device; no GPIO here)", " ".join(argv[1:]))

    def cmd_adc(self, argv: list[str], r: Reply) -> None:
        if len(argv) < 2:
            log.info("adc CHANNEL")
            return
        chan = int(argv[1], 0) if argv[1].lstrip("-").isdigit() else 0
        mv = self.adc_mv(chan)
        r.add("ADC%u raw=%u mv=%u\r\n" % (chan, mv * 4095 // 3300, mv))

    def cmd_mem(self, argv: list[str], r: Reply) -> None:
        if len(argv) < 2:
            r.add("mem HEXADDR [NWORDS<=16]  (raw 32-bit reads, debug only)\r\n")
            return
        try:
            addr = int(argv[1], 16) & ~3
        except ValueError:
            addr = 0
        n = max(1, min(16, int(argv[2]) if len(argv) > 2 and argv[2].isdigit() else 4))
        words = " ".join("%08x" % ((addr + 4 * i) * 2654435761 & 0xFFFFFFFF) for i in range(n))
        r.add("[mem] %08x: %s\r\n" % (addr, words))

    def cmd_reboot(self, argv: list[str], r: Reply) -> None:
        log.info("rebooting (restoring boot GPIO config)")
        self.reboot("reboot", REBOOT_DOWN_S)

    # ---- wifi / hook / ota / tok
    def cmd_wifi(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 4 and argv[1] == "set":
            self.ssid = argv[2]
            self.store.kv_set("db_ssid", argv[2])
            self.store.kv_set("db_pass", argv[3])
            log.info("[wifi] saved ssid '%s' (%u-char password), connecting", argv[2], len(argv[3]))
        elif len(argv) >= 2 and argv[1] == "clear":
            self.ssid = ""
            self.store.kv_set("db_ssid", "")
            self.store.kv_set("db_pass", "")
            log.info("[wifi] credentials cleared")
        elif len(argv) >= 2 and argv[1] == "connect":
            log.info("[wifi] connect -> 0")
        else:
            r.add("[wifi] ssid '%s' state %d rssi %d ip %s got_ip %d\r\n" % (self.ssid, 4 if self.got_ip else 0,
                                                                            -52 if self.got_ip else 0, self.ip, 1 if self.got_ip else 0))

    def cmd_hook(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 3 and argv[1] == "set":
            self.hook = argv[2][:127]
            self.store.kv_set("db_hook", self.hook)
        elif len(argv) >= 2 and argv[1] == "test":
            self.pend_test = True
        elif len(argv) >= 2 and argv[1] == "ring":
            self.ring_count += 1
            self.pend_ring = True
        elif len(argv) >= 2 and argv[1] == "motion":
            self.motion_count += 1
            self.pend_motion = True
        r.add("[hook] %s  (hook set HOST:PORT/PATH | test | ring | motion)\r\n" % (self.hook or "not set"))

    def cmd_ota(self, argv: list[str], r: Reply) -> None:
        if len(argv) < 2:
            log.info("ota http://HOST:PORT/app_pack.rbl")
            return
        if not self.got_ip:
            log.info("[ota] no network")
            return
        url = argv[1][:159]
        log.info("[ota] downloading %s", url)
        if self.ota_running:
            log.info("[ota] thread failed -1")
            return
        self.ota_running = True

        def run() -> None:
            ret, size = 0, 0
            try:
                with urllib.request.urlopen(url, timeout=30) as resp:
                    while True:
                        chunk = resp.read(65536)
                        if not chunk:
                            break
                        size += len(chunk)
            except Exception as e:
                log.info("[ota] http error: %s", e)
                ret = -4
            path = urllib.parse.urlparse(url).path
            if ret == 0 and (size == 0 or not path.endswith(".rbl")):
                ret = -3
            log.info("[ota] result %d%s", ret, " (rebooting)" if ret == 0 else "")
            self.ota_running = False
            if ret == 0:
                m = re.search(r"doorbell-fw_([0-9A-Za-z.+-]+)_app_pack\.rbl$", path)
                if m:
                    self.fw = m.group(1)
                    self.store.set("fw", "", self.fw)
                log.info("[ota] %u bytes written to the ota partition, bootloader installs fw %s", size, self.fw)
                self.reboot("ota", OTA_DOWN_S)

        threading.Thread(target=run, name="ota", daemon=True).start()

    def cmd_tok(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 3 and argv[1] == "set":
            self.token = argv[2][:47]
            self.store.kv_set("db_token", self.token)
            log.info("[tok] token set (%u chars); TCP channel active after reboot", len(self.token))
        elif len(argv) >= 2 and argv[1] == "clear":
            self.token = ""
            self.store.kv_set("db_token", "")
            log.info("[tok] cleared")
        else:
            log.info("[tok] %s  (tok set SECRET | clear)", "set" if self.token else "not set")

    # ---- pir / motion
    def cmd_pir(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2 and argv[1] in ("on", "off"):
            self.pir_on = argv[1] == "on"
            self.store.kv_set("db_pir_on", "1" if self.pir_on else "0")
            if self.pir_on:
                self.pir_armed_ms = self.clock.now_ms()
            r.add("[pir] %s (GPIO50=%d, IRQ %s, edges ignored for %u ms)\r\n" % (
                argv[1], 1 if self.pir_on else 0, "enabled" if self.pir_on else "disabled", PIR_WARMUP_MS if self.pir_on else 0))
        elif len(argv) >= 3 and argv[1] == "sens":
            v = int(argv[2]) if argv[2].isdigit() else 0
            if v > PIR_SENS_MAX:
                r.add("[pir] sens must be 0-%u (stock used 0/1/2, default 1)\r\n" % PIR_SENS_MAX)
                return
            self.pir_sens = v
            self.store.kv_set("db_pir_sens", str(v))
            r.add("[pir] sens %u -> GPIO25=%u GPIO26=%u%s\r\n" % (v, v & 1, (v >> 1) & 1, "" if self.pir_on else " (applied when `pir on`)"))
        elif len(argv) >= 3 and argv[1] == "cooldown":
            v = int(argv[2]) if argv[2].isdigit() else 0
            if v < PIR_CD_MIN_MS or v > PIR_CD_MAX_MS:
                r.add("[pir] cooldown must be %u-%u ms\r\n" % (PIR_CD_MIN_MS, PIR_CD_MAX_MS))
                return
            self.pir_cd_ms = v
            self.store.kv_set("db_pir_cd", str(v))
            r.add("[pir] cooldown %u ms\r\n" % v)
        elif len(argv) >= 2 and argv[1] != "status":
            r.add("pir on|off | sens 0-3 | cooldown MS | status\r\n")
        else:
            now = self.clock.now_ms()
            ago = now - self.pir_last_ms if self.pir_count else 0
            left = self.pir_cd_ms - (now - self.pir_last_ms) if self.pir_count and now - self.pir_last_ms < self.pir_cd_ms else 0
            r.add("[pir] %s sens=%u (GPIO25=%u GPIO26=%u) cooldown=%u ms count=%u last=%u ms ago (cooldown left %u ms) "
                  "edges=%u ignored=%u missed=%u GPIO24=%d GPIO50=%d\r\n" % (
                      "on" if self.pir_on else "off", self.pir_sens, self.pir_sens & 1, (self.pir_sens >> 1) & 1, self.pir_cd_ms,
                      self.pir_count, ago, left, self.pir_edges, self.pir_ignored, self.pir_missed, 0, 1 if self.pir_on else 0))

    def cmd_motion(self, argv: list[str], r: Reply) -> None:
        if len(argv) < 2 or argv[1] != "test":
            r.add("motion test   (simulated PIR edge through the ISR handler: cooldown, count, HA report)\r\n")
            return
        r.add(self.motion_text(self.pir_trigger()))

    # ---- led / night / sys / ring
    def cmd_led(self, argv: list[str], r: Reply) -> None:
        which = 0 if len(argv) >= 3 and argv[1] == "white" else 1 if len(argv) >= 3 and argv[1] == "ring" else -1
        if which < 0 or argv[2] not in ("on", "off", "auto"):
            r.add("led white|ring on|off|auto   (on/off = manual override of the night light / ring pattern, auto = release)\r\n")
            return
        state = -1 if argv[2] == "auto" else 1 if argv[2] == "on" else 0
        self.led_manual(which, state)
        r.add("[led] %s %s (GPIO %d%s)\r\n" % (argv[1], argv[2], PIN_LED_WHITE if which == 0 else PIN_LED_RING,
                                             ", automatic" if state < 0 else ", manual override: `led ... auto` to release"))

    def cmd_night(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 3 and argv[1] == "auto":
            self.night_auto = argv[2] == "on"
            self.store.kv_set("db_night_auto", "1" if self.night_auto else "0")
            if not self.night_auto and self.night_motion_until:
                self.night_motion_until = 0
                self.white_set(NIGHT_SRC_MOTION, False)
            r.add("[night] auto %s\r\n" % ("on" if self.night_auto else "off"))
        elif len(argv) >= 3 and argv[1] == "ring":
            self.night_ring = argv[2] == "on"
            self.store.kv_set("db_night_ring", "1" if self.night_ring else "0")
            r.add("[night] ring indicator %s\r\n" % ("on" if self.night_ring else "off"))
        elif len(argv) >= 3 and argv[1] == "hyst":
            self.night_hyst = int(argv[2]) if argv[2].isdigit() else 0
            self.store.kv_set("db_night_hyst", str(self.night_hyst))
            r.add("[night] hysteresis %u mV (off below %u)\r\n" % (self.night_hyst, max(0, self.dark_mv - self.night_hyst)))
        elif len(argv) >= 2 and argv[1] == "status":
            now = self.clock.now_ms()
            left = (self.night_motion_until - now + 999) // 1000 if self.night_motion_until and self.night_motion_until > now else 0
            r.add("[night] auto %d ring %d hyst %u | light %u mV (last %u, dark > %u) cam %d cam_dark %d samples %u motion_left %u s"
                  " | white src 0x%x manual %d out %d | ring manual %d out %d wifi %d fx %d\r\n" % (
                      self.night_auto, self.night_ring, self.night_hyst, self.light_read_mv(), self.night_last_mv, self.dark_mv,
                      int(self.cam.open), int(self.cam_dark), self.night_samples, left, self.white_mask, self.white_manual,
                      int(self.white_out), self.ring_manual, int(self.ring_out), int(self.got_ip), int(self.fx_running)))
        else:
            r.add("night auto on|off | ring on|off | hyst MV | status   (auto %d ring %d hyst %u)\r\n" % (
                self.night_auto, self.night_ring, self.night_hyst))

    def cmd_sys(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2 and argv[1] == "info":
            up = self.uptime_ms() // 1000
            rr = RESET_REASONS.get(self.boot_reason, 0xFF)
            r.add("[sys] fw %s uptime %us (%uh%02um) ip %s wifi %d heap %u B reset 0x%x (%s) light %u mV batt %u mV cam %d\r\n" % (
                self.fw, up, up // 3600, (up // 60) % 60, self.ip, int(self.got_ip), 126000 + random.randint(-800, 800), rr,
                self.boot_reason, self.light_read_mv(), self.adc_mv(9), int(self.cam.open)))
        else:
            r.add("sys info\r\n")

    def cmd_ring(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2 and argv[1] == "test":
            mv = self.light_read_mv()
            self.ringfx()
            r.add("[ring] effects started (light %u mV, dark above %u -> %s)\r\n" % (
                mv, self.dark_mv, "white light on" if mv > self.dark_mv else "day, no light"))
        elif len(argv) >= 3 and argv[1] == "dark":
            self.dark_mv = int(argv[2]) if argv[2].isdigit() else 0
            self.store.kv_set("db_dark_mv", str(self.dark_mv))
            r.add("[ring] dark threshold %u mV\r\n" % self.dark_mv)
        elif len(argv) >= 3 and argv[1] == "chan":
            self.light_chan = int(argv[2]) if argv[2].isdigit() else 0
            self.store.kv_set("db_light_chan", str(self.light_chan))
            r.add("[ring] light sensor on ADC%u, now %u mV\r\n" % (self.light_chan, self.light_read_mv()))
        elif len(argv) >= 3 and argv[1] == "fx":
            self.fx_enabled = argv[2] == "on"
            self.store.kv_set("db_fx", "1" if self.fx_enabled else "0")
            r.add("[ring] local effects %s\r\n" % ("on" if self.fx_enabled else "off"))
        else:
            r.add("ring test | dark MV | chan ADC | fx on|off   (light now %u mV, dark > %u, fx %d)\r\n" % (
                self.light_read_mv(), self.dark_mv, int(self.fx_enabled)))

    # ---- pwr
    def cmd_pwr(self, argv: list[str], r: Reply) -> None:
        def onoff(s: str) -> Optional[bool]:
            return True if s in ("on", "1") else False if s in ("off", "0") else None

        now = self.clock.now_ms()
        if len(argv) < 2 or argv[1] == "status":
            adc = self.adc_mv(self.pwr_bchan)
            mv = adc * self.pwr_bratio // 100
            hold = (POWER_BOOT_HOLD_MS - self.uptime_ms()) // 1000 if self.uptime_ms() < POWER_BOOT_HOLD_MS else 0
            nxt, why = self.wake_interval()
            r.add("[pwr] batt %u mV %u%% (adc%u %u mV x%u.%02u) usb %d chg %d | sleep %s force %s idle %u s wake_pir %s%s snap_min %u"
                  " | sched %s -> next %u min (%s) | up %u s hold %u s idle %u s busy %d%s | boot %s%s%s\r\n" % (
                      mv, self.batt_pct(mv, bool(self.charging)), self.pwr_bchan, adc, self.pwr_bratio // 100, self.pwr_bratio % 100,
                      self.usb, self.charging, "on" if self.pwr_sleep else "off", "on" if self.pwr_force else "off", self.pwr_idle_s,
                      "on" if self.pwr_wake_pir else "off", " (FORCED OFF: battery)" if self.pwr_wake_pir and self.low_batt else "",
                      self.pwr_snap_min, self.sched_str(), nxt, why, self.uptime_ms() // 1000, hold,
                      (now - self.last_activity_ms) // 1000, int(self.busy()), " sleepnow pending" if self.sleep_at_ms else "",
                      self.boot_reason, " wake=" if self.wake_reason else "", self.wake_reason))
            return
        sub = argv[1]
        if sub == "sleep" and len(argv) >= 4 and argv[2] == "force" and onoff(argv[3]) is not None:
            self.pwr_force = bool(onoff(argv[3]))
            self.store.kv_set("db_pwr_force", "1" if self.pwr_force else "0")
            r.add("[pwr] sleep force %s (sleep even on USB)\r\n" % ("on" if self.pwr_force else "off"))
        elif sub == "sleep" and len(argv) >= 3 and onoff(argv[2]) is not None:
            self.pwr_sleep = bool(onoff(argv[2]))
            self.store.kv_set("db_pwr_sleep", "1" if self.pwr_sleep else "0")
            if not self.pwr_sleep:
                self.sleep_at_ms = 0
            r.add("[pwr] automatic sleep %s%s\r\n" % ("on" if self.pwr_sleep else "off",
                                                     " (first sleep after the 120 s boot hold)" if self.pwr_sleep and self.uptime_ms() < POWER_BOOT_HOLD_MS else ""))
        elif sub == "idle" and len(argv) >= 3:
            v = max(POWER_IDLE_MIN_S, int(argv[2]) if argv[2].isdigit() else 0)
            self.pwr_idle_s = v
            self.store.kv_set("db_pwr_idle", str(v))
            r.add("[pwr] idle %u s\r\n" % v)
        elif sub == "wake_pir" and len(argv) >= 3 and onoff(argv[2]) is not None:
            self.pwr_wake_pir = bool(onoff(argv[2]))
            self.store.kv_set("db_pwr_wpir", "1" if self.pwr_wake_pir else "0")
            r.add("[pwr] PIR wake %s\r\n" % ("on" if self.pwr_wake_pir else "off"))
        elif sub == "snap_min" and len(argv) >= 3:
            v = min(POWER_SNAP_MAX_MIN, int(argv[2]) if argv[2].isdigit() else 0)
            self.pwr_snap_min = v
            self.store.kv_set("db_pwr_snap", str(v))
            r.add("[pwr] rtc wake every %u min%s%s\r\n" % (v or POWER_KEEPALIVE_MIN, "" if v else " (keepalive only)",
                                                           " (used only with `pwr sched off`; the schedule is on)" if self.sched_on else ""))
        elif sub == "sleepnow":
            if self.sleeping:
                r.add("[pwr] sleep sequence already running\r\n")
                return
            self.sleep_at_ms = now + POWER_SLEEPNOW_GRACE_MS or 1
            r.add("[pwr] sleeping in %u s: wake on button%s, usb plug, uart rx, rtc in %u min (cancel: pwr sleep off)\r\n" % (
                POWER_SLEEPNOW_GRACE_MS // 1000, ", pir" if self.pwr_wake_pir and not self.low_batt else "", self.wake_interval()[0]))
        elif sub == "chan" and len(argv) >= 3:
            v = int(argv[2]) if argv[2].isdigit() else 99
            if v < 16:
                self.pwr_bchan = v
                self.store.kv_set("db_pwr_bchan", str(v))
            r.add("[pwr] battery on ADC%u: %u mV x%u.%02u = %u mV\r\n" % (self.pwr_bchan, self.adc_mv(self.pwr_bchan),
                                                                         self.pwr_bratio // 100, self.pwr_bratio % 100, self.power_batt_mv()))
        elif sub == "ratio" and len(argv) >= 3:
            v = int(argv[2]) if argv[2].isdigit() else 0
            if 100 <= v <= 400:
                self.pwr_bratio = v
                self.store.kv_set("db_pwr_bratio", str(v))
            r.add("[pwr] divider ratio x%u.%02u -> %u mV\r\n" % (self.pwr_bratio // 100, self.pwr_bratio % 100, self.power_batt_mv()))
        elif sub == "sched":
            if len(argv) == 3 and argv[2] in ("on", "off"):
                self.sched_on = argv[2] == "on"
                joined = "%02d:%02d %02d:%02d %u %u" % (self.day_start // 60, self.day_start % 60, self.day_end // 60, self.day_end % 60,
                                                        self.day_min, self.night_min)
                self.store.kv_set("db_pwr_sched", joined if self.sched_on else "off")
            elif len(argv) >= 6:
                joined = " ".join(argv[2:6])
                if not self.sched_parse(joined):
                    r.add("pwr sched DAY_START DAY_END DAY_MIN NIGHT_MIN (HH:MM HH:MM 1-1440 1-1440, e.g. 07:00 22:00 15 60) | sched off | sched on\r\n")
                    return
                self.store.kv_set("db_pwr_sched", joined)
            elif len(argv) != 2:
                r.add("pwr sched DAY_START DAY_END DAY_MIN NIGHT_MIN | sched off | sched on | sched\r\n")
                return
            nxt, why = self.wake_interval()
            r.add("[pwr] sched %s -> next rtc wake in %u min (%s)\r\n" % (self.sched_str(), nxt, why))
        elif sub == "policy":
            if len(argv) >= 3:
                if argv[2] == "pull":
                    self.policy_fetch(True)
                elif argv[2] in ("auto", "off") or argv[2].startswith("http://"):
                    self.policy_cfg = argv[2][:95]
                    self.store.kv_set("db_pwr_policy", self.policy_cfg)
                else:
                    r.add("pwr policy [auto | off | http://IP[:port]/path | pull]   (auto = http://<hook host>:<hook port>%s)\r\n" % POLICY_PATH)
                    return
            url = self.policy_url()
            src = "off" if self.policy_cfg == "off" else (url + " (auto)" if url and self.policy_cfg == "auto" else url or "none (auto: hook not set)")
            r.add("[policy] %s | last: %s%s%s (%s)%s%u s ago\r\n" % (
                src, self.policy_result, " rev " if self.policy_rev else "", self.policy_rev, self.policy_detail,
                " " if self.policy_at_ms else " never, ", max(0, self.clock.now_ms() - self.policy_at_ms) // 1000 if self.policy_at_ms else 0))
        else:
            r.add("pwr status | sleep on|off | sleep force on|off | idle S | wake_pir on|off | snap_min M | sched START END DAY NIGHT|off"
                  " | policy [URL|auto|off|pull] | sleepnow | chan ADC | ratio PCT\r\n")

    # ---- vol / log (policy.c)
    def cmd_vol(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2:
            if not argv[1].isdigit() or int(argv[1]) > 100:
                r.add("vol [0-100]   (speaker volume: chime and talk `gain auto`; now %u%% = dac gain 0x%x)\r\n" % (
                    self.vol, (self.vol * VOL_GAIN_MAX + 50) // 100))
                return
            self.vol = int(argv[1])
            self.store.kv_set("db_vol", str(self.vol))
        r.add("[vol] %u%% -> dac gain 0x%x (chime + talk auto; `spk cfg gain N` overrides raw)\r\n" % (self.vol, (self.vol * VOL_GAIN_MAX + 50) // 100))

    def cmd_log(self, argv: list[str], r: Reply) -> None:
        n = 300
        if len(argv) >= 2:
            if argv[1] == "stat":
                r.add("[log] %u bytes written, %u in the %u-byte ring (log [N<=%u] | GET /log?t=TOKEN)\r\n" % (
                    ramlog.total, min(ramlog.total, APPLOG_SIZE), APPLOG_SIZE, LOG_REPLY_MAX))
                return
            n = int(argv[1]) if argv[1].isdigit() else 0
            if n <= 0 or n > LOG_REPLY_MAX:
                n = LOG_REPLY_MAX
        r.text = (r.text + ramlog.tail(n).decode(errors="replace"))[:REPLY_MAX]   # app_reply_raw(): not logged again

    # ---- cam / rtsp
    def cmd_cam(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2 and argv[1] == "probe":
            r.add("[cam] probe started (0 rail pins), see UART\r\n")
        elif len(argv) >= 2 and argv[1] == "open":
            h264 = argv[-1] == "h264"
            dims = len(argv) - 2 - (1 if h264 else 0)
            w, h = (int(argv[2]), int(argv[3])) if dims >= 2 and argv[2].isdigit() and argv[3].isdigit() else (1280, 720)
            ret = self.cam_open(w, h, h264)
            r.add("[cam] open %ux%u %s -> %d%s\r\n" % (w, h, "h264" if h264 else "jpeg", ret, " (open)" if self.cam.open else ""))
        elif len(argv) >= 2 and argv[1] == "close":
            self.cam_close()
            r.add("[cam] closed\r\n")
        elif len(argv) >= 2 and argv[1] == "stat":
            c = self.cam
            r.add("[cam] open %d %s (mode %s) frames %u h264 %u dropped %u last jpeg %u bytes %ux%u seq %u age %u ms\r\n" % (
                int(c.open), ("h264" if c.h264 else "jpeg") if c.open else "-", ("auto", "jpeg", "h264")[c.pref], c.frames,
                c.h264_frames, c.drop, len(c.jpeg), c.w, c.h, c.seq, self.clock.now_ms() - c.last_ms if c.last_ms else 0))
        elif len(argv) >= 3 and argv[1] == "mode":
            if argv[2] not in ("auto", "jpeg", "h264"):
                r.add("cam mode auto|jpeg|h264\r\n")
                return
            self.cam.pref = ("auto", "jpeg", "h264").index(argv[2])
            self.store.kv_set("db_cam_mode", argv[2])
            r.add("[cam] mode %s (applies at the next open/switch)\r\n" % argv[2])
        elif len(argv) >= 4 and argv[1] == "snap":
            if not self.cam.jpeg:
                r.add("[cam] no frame yet\r\n")
                return
            r.add("[cam] sending %u bytes to %s:%s\r\n" % (len(self.cam.jpeg), argv[2], argv[3]))
            threading.Thread(target=self._tcp_push, args=(argv[2], argv[3], self.cam.jpeg), daemon=True).start()
        else:
            r.add("cam open [W H] [h264] | close | stat | mode auto|jpeg|h264 | snap HOST PORT | http://IP:8080/snapshot.jpg /stream"
                  " | probe [PWR_PIN ...] | probe scan | probe rails on|off | probe bb SCL SDA\r\n")

    @staticmethod
    def _tcp_push(host: str, port: str, data: bytes) -> None:
        try:
            with socket.create_connection((host, int(port)), timeout=5) as s:
                s.sendall(data)
        except (OSError, ValueError) as e:
            log.info("[cam] snap to %s:%s failed: %s", host, port, e)

    def cmd_rtsp(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2 and argv[1] == "on":
            ret = self.rtsp.start(self.rtsp_port)
            self.rtsp_on = True
            self.store.kv_set("db_rtsp", "1")
            r.add("[rtsp] on, port %u -> %d (persisted; clients switch the camera to H.264 on demand)\r\n" % (self.rtsp_port, ret))
        elif len(argv) >= 2 and argv[1] == "off":
            self.rtsp.stop()
            self.rtsp_on = False
            self.store.kv_set("db_rtsp", "0")
            r.add("[rtsp] off (persisted; server stops within ~1 s, camera released if in H.264)\r\n")
        elif len(argv) >= 2 and argv[1] == "status":
            r.add(self.rtsp.status())
            r.add("[rtsp] audio: voc %s%s aec %s | clients 0 | mic in 0 frames 0 B | back in 0 pkts 0 B | voc fails 0\r\n"
                  % ("on" if self.voc_on else "off", (" (manual)" if self.voc_manual else " (rtsp)") if self.voc_on else "",
                     "on" if self.talk_aec else "off"))
        elif len(argv) >= 3 and argv[1] == "port":
            p = int(argv[2]) if argv[2].isdigit() else 0
            if p == 0 or p > 65535:
                r.add("[rtsp] bad port\r\n")
                return
            self.rtsp_port = p
            self.store.kv_set("db_rtsp_port", argv[2])
            r.add("[rtsp] port %u (takes effect at the next `rtsp off` + `rtsp on`, or reboot)\r\n" % p)
        elif len(argv) >= 4 and argv[1] == "q":
            imb = int(argv[2]) if argv[2].isdigit() else 0
            pmb = int(argv[3]) if argv[3].isdigit() else 0
            if imb < 10 or imb > 290 or pmb < 4 or pmb > 1000:
                r.add("[rtsp] q IMB(10..290) PMB(4..1000) bits per macroblock; 720p = 3600 MB, 15 fps, IDR every 2 s\r\n")
                return
            self.h264_imb, self.h264_pmb = imb, pmb
            self.store.kv_set("db_h264_q", "%u %u" % (imb, pmb))
            applied = self.cam.open and self.cam.h264
            r.add("[rtsp] q imb %u pmb %u (~%u kbit/s nominal)%s\r\n" % (imb, pmb, (imb + 29 * pmb) * 3600 // 2000,
                                                                       ", applied" if applied else ", applied at the next H.264 open"))
        elif len(argv) >= 2 and argv[1] == "idr":
            r.add("[rtsp] IDR requested\r\n")
        else:
            r.add("rtsp on|off | status | port N | q IMB PMB | idr   (now %s, port %u, q %u/%u, cam mode %d)\r\n" % (
                "running" if self.rtsp.running() else "stopped", self.rtsp_port, self.h264_imb, self.h264_pmb, self.cam.pref))

    # ---- spk / mic / talk
    def cmd_spk(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2 and argv[1] == "tone":
            hz = int(argv[2]) if len(argv) >= 3 and argv[2].isdigit() else 1000
            ms = int(argv[3]) if len(argv) >= 4 and argv[3].isdigit() else 1000
            r.add("[spk] tone %u Hz %u ms\r\n" % (hz, ms))
            if self.spk_busy:
                r.add("[spk] busy\r\n")
            else:
                self.spk_play(ms, "tone %u Hz" % hz, hz)
        elif len(argv) >= 3 and argv[1] == "play":
            clips = {"beepa": (0x601000, 23180), "beepb": (0x607000, 26104), "bell": (0x60E000 + 16, 66450 - 16)}
            if argv[2] in clips:
                addr, ln = clips[argv[2]]
            elif len(argv) >= 4:
                try:
                    addr, ln = int(argv[2], 16), int(argv[3])
                except ValueError:
                    addr, ln = 0, 0
            else:
                addr, ln = 0, 0
            if not addr:
                r.add("spk play beepa|beepb|bell|HEXADDR LEN\r\n")
                return
            r.add("[spk] play 0x%x len %u\r\n" % (addr, ln))
            if self.spk_busy:
                r.add("[spk] busy\r\n")
            else:
                self.spk_play(ln * 1000 // (SPK_RATE * 2), "play 0x%x" % addr)
        elif len(argv) >= 4 and argv[1] == "net":
            if self.spk_busy:
                r.add("[spk] busy\r\n")
                return
            r.add("[spk] stream from %s:%s\r\n" % (argv[2], argv[3]))
            self.spk_play(3000, "net %s:%s (not connected: simulator)" % (argv[2], argv[3]))
        elif len(argv) >= 2 and argv[1] == "stop":
            r.add("[spk] stop requested\r\n")
        elif len(argv) >= 3 and argv[1] == "pa":
            self.pa_pin = -1 if argv[2] == "off" else int(argv[2]) if argv[2].isdigit() else 0
            r.add("[spk] pa pin %d\r\n" % self.pa_pin)
        elif len(argv) >= 2 and argv[1] == "cfg":
            for i in range(2, len(argv) - 1, 2):
                if argv[i] == "gain":
                    try:
                        self.spk_gain_raw = -1 if argv[i + 1] == "auto" else int(argv[i + 1], 0)
                    except ValueError:
                        pass
            r.add("[spk] cfg chl 0 mode 0 gain 0x%x%s rate %u clk xtal pa %d\r\n" % (
                self.spk_gain(), " (raw override)" if self.spk_gain_raw >= 0 else " (vol)", SPK_RATE, self.pa_pin))
        else:
            r.add("spk tone [HZ] [MS] | play beepa|beepb|bell|ADDR LEN | net HOST PORT | stop | pa PIN|off | cfg [chl L|R|LR] [mode diff|se]"
                  " [gain N] [rate HZ] [clk xtal|apll]  (busy %d)\r\n" % int(self.spk_busy))

    def cmd_mic(self, argv: list[str], r: Reply) -> None:
        if len(argv) >= 2 and argv[1] == "cfg":
            r.add("[mic] cfg mic 1 mode 0 gain 0x%x chl 0 rate %u clk xtal\r\n" % (self.talk_micgain, SPK_RATE))
        elif len(argv) >= 5 and argv[1] == "rec":
            if self.mic_busy:
                r.add("[mic] busy\r\n")
                return
            ms = min(60000, int(argv[2]) if argv[2].isdigit() else 0)
            r.add("[mic] rec %u ms -> %s:%s (raw 32-bit FIFO words, lo16 = L, hi16 = R)\r\n" % (ms, argv[3], argv[4]))
            data = synth_pcm16(TALK_MIC_RATE, ms / 1000, 440.0)
            words = b"".join(struct.pack("<hh", s, 0) for s in struct.unpack("<%dh" % (len(data) // 2), data))
            threading.Thread(target=self._tcp_push, args=(argv[3], argv[4], words), daemon=True).start()
        elif len(argv) >= 2 and argv[1] == "level":
            ms = int(argv[2]) if len(argv) >= 3 and argv[2].isdigit() else 2000
            hz = int(argv[3]) if len(argv) >= 4 and argv[3].isdigit() else 1000
            n = min(ms, 4000) * TALK_MIC_RATE // 1000
            r.add("[mic] %u samples in %u ms | lo16: peak 212 rms 90 dc -3 tone%u 1 | hi16: peak 6 rms 2 dc 0 tone%u 0\r\n" % (n, min(ms, 4000), hz, hz))
        else:
            r.add("mic rec MS HOST PORT | mic level [MS] [HZ] | cfg [mic 1|2|both] [mode diff|se] [gain N] [chl L|R|LR] [rate HZ] [clk xtal|apll]\r\n")

    def cmd_talk(self, argv: list[str], r: Reply) -> None:
        t = self.talk
        gain = self.talk_gain or self.spk_gain()
        if len(argv) >= 2 and argv[1] == "status":
            st = ("speaking" if t.speaking else "") + (("+listening" if t.speaking else "listening") if t.listening else "") \
                + ("idle" if not t.speaking and not t.listening else "")
            r.add("[talk] %s engine %s | speak: %u calls, last %u ms (in %u Hz), %u bytes in | mic: %u calls, %u bytes out, %u Hz | last http %d"
                  " | gain %s0x%x micgain 0x%x max %u s\r\n" % (st, "voc" if self.voc_on else "raw", t.speak_calls, t.speak_last_ms,
                                                              t.speak_last_rate, t.speak_in_bytes, t.mic_calls, t.mic_out_bytes,
                                                              VOC_RATE if self.voc_on else TALK_MIC_RATE, t.last_err,
                                                              "" if self.talk_gain else "auto=", gain, self.talk_micgain, self.talk_max_s))
            r.add(self.voc_status())
        elif len(argv) >= 3 and argv[1] == "aec":
            if argv[2] not in ("on", "off"):
                r.add("[talk] aec on|off\r\n")
                return
            self.talk_aec = argv[2] == "on"
            self.store.kv_set("db_talk_aec", "1" if self.talk_aec else "0")
            r.add("[talk] aec %s%s\r\n" % (argv[2], " (applied to the running voc engine)" if self.voc_on else " (next voc start)"))
        elif len(argv) >= 3 and argv[1] == "vocmic":
            try:
                g = int(argv[2], 0)
            except ValueError:
                g = 0x40
            if g > 0x3F:
                r.add("[talk] vocmic 0..0x3f (ADC gain for the voc engine / RTSP audio; 0 = default 0x3f)\r\n")
                return
            self.voc_mic = g
            self.store.kv_set("db_voc_mic", str(g))
            r.add("[talk] voc mic gain 0x%x%s\r\n" % (g or 0x3F, " (applied)" if self.voc_on else ""))
        elif len(argv) >= 3 and argv[1] == "voc":
            if argv[2] == "on":
                rc = self.voc_start(manual=True)
                r.add("[talk] voc start -> %d%s\r\n" % (rc, " (manual: mic frames dropped until an RTSP audio client joins; /speak and /mic.wav go through it)"
                                                        if rc == 0 else " (raw engine busy: chime, spk, mic, /speak or /mic.wav running)"))
            elif argv[2] == "off":
                self.voc_stop()
                r.add("[talk] voc stopped (an RTSP audio client restarts it within 0.5 s)\r\n")
            else:
                r.add("[talk] voc on|off\r\n")
        elif len(argv) >= 3 and argv[1] == "gain":
            try:
                g = 0 if argv[2] == "auto" else int(argv[2], 0)
            except ValueError:
                g = 0x40
            if g > 0x3F:
                r.add("[talk] gain 0..0x3f or auto (0x1c = loudest clean level, above 0x1c the amplifier clips)\r\n")
                return
            self.talk_gain = g
            self.store.kv_set("db_talk_gain", str(g))
            r.add("[talk] speaker gain %s0x%x\r\n" % ("" if g else "auto = spk cfg ", g or self.spk_gain()))
        elif len(argv) >= 3 and argv[1] == "micgain":
            try:
                g = int(argv[2], 0)
            except ValueError:
                g = 0x40
            if g > 0x3F:
                r.add("[talk] micgain 0..0x3f\r\n")
                return
            self.talk_micgain = g
            self.store.kv_set("db_talk_micgain", str(g))
            r.add("[talk] mic gain 0x%x (next /mic.wav)\r\n" % g)
        elif len(argv) >= 3 and argv[1] == "max":
            s = int(argv[2]) if argv[2].isdigit() else 0
            if s < 1 or s > TALK_MAX_LIMIT_S:
                r.add("[talk] max 1..%u seconds\r\n" % TALK_MAX_LIMIT_S)
                return
            self.talk_max_s = s
            self.store.kv_set("db_talk_max", str(s))
            r.add("[talk] max %u s per /speak\r\n" % s)
        elif len(argv) >= 2 and argv[1] == "stop":
            t.abort = True
            r.add("[talk] stop requested (speaking %d listening %d)\r\n" % (int(t.speaking), int(t.listening)))
        else:
            r.add("talk status | gain N|auto | micgain N | max SECONDS | stop | aec on|off | vocmic N | voc on|off   (http://IP:8080/speak /mic.wav /talk; rtsp audio = voc engine)\r\n")

    # ---- voc engine (audio_voc.c): the SDK voice mode with AEC behind the RTSP audio tracks
    def voc_start(self, manual: bool = False) -> int:
        if self.voc_on:
            return 0
        if self.spk_busy or self.mic_busy or self.talk.speaking or self.talk.listening:
            return -1
        self.voc_on, self.voc_manual = True, manual
        self.voc_starts += 1
        log.info("[voc] on: 8 kHz G711A, aec %s, mic gain 0x%x spk gain 0x%x, ring 10 x 320 B (prefill 4)",
                 "on" if self.talk_aec else "off", self.voc_mic or 0x3F, self.spk_gain())
        return 0

    def voc_stop(self) -> None:
        if self.voc_on:
            log.info("[voc] off")
        self.voc_on = self.voc_manual = False

    def voc_status(self) -> str:
        return ("[voc] %s aec %s mic 0x%x spk 0x%x | mic 0 cb 0 B | spk 0 frames 0 B drop-tap 0 err 0 | tap - 0 frames 0 late | mictap - 0 ovr | starts %u\r\n"
                % ("on" if self.voc_on else "off", "on" if self.talk_aec else "off", self.voc_mic or 0x3F, self.spk_gain(), self.voc_starts))

    # ---- sd
    def cmd_sd(self, argv: list[str], r: Reply) -> None:
        sd = self.sd
        sub = argv[1] if len(argv) >= 2 else "status"
        ts = _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        if sub == "status":
            r.add("[sd]  card %s %s cap %u MB free %u MB auto %s saved %u fail %u drop %u last %s\r\n" % (
                "present" if sd.present else "absent", "mounted" if sd.mounted else "unmounted", sd.cap_mb, sd.free_mb,
                "on" if self.sd_auto else "off", sd.saved, sd.fail, sd.drop, sd.last_file or "-"))
            r.add("[sd] pins 2/3/4 pwr 6 time %s (sim, ntp 1/1 pool.ntp.org tz 0): %s\r\n" % (ts, sd.result))
        elif sub == "mount":
            sd.mounted = sd.present
            sd.result = "mounted %u MB" % sd.cap_mb if sd.mounted else "no card"
            r.add("[sd] %s\r\n" % sd.result)
        elif sub == "umount":
            sd.mounted = False
            sd.result = "unmounted"
            r.add("[sd] %s\r\n" % sd.result)
        elif sub == "format":
            now = self.clock.now_ms()
            if len(argv) >= 3 and sd.fmt_token and now - sd.fmt_token_ms < 120000 and argv[2] == "%04x" % sd.fmt_token:
                sd.fmt_token = 0
                sd.files.clear()
                sd.saved, sd.free_mb, sd.last_file = 0, sd.cap_mb - 168, ""
                sd.result = "formatted"
                r.add("[sd] formatting (FM_ANY) in the background; poll `sd status`\r\n")
            else:
                sd.fmt_token = ((now * 2654435761) >> 16) & 0xFFFF or 1
                sd.fmt_token_ms = now
                r.add("[sd] ERASES THE CARD. Confirm within 120 s with: sd format %04x\r\n" % sd.fmt_token)
        elif sub == "ls":
            if not sd.mounted:
                r.add("[sd] not mounted\r\n")
                return
            d = argv[2] if len(argv) >= 3 else "/doorbell"
            skip = int(argv[3]) if len(argv) >= 4 and argv[3].isdigit() else 0
            path = "1:/" + d.lstrip("/")
            shown = 0
            for name, size in sd.files[skip:skip + 6]:
                r.add("[sd]   %s %u\r\n" % (name.split("/")[-1], size))
                shown += 1
            r.add("[sd] %s: %u entries, shown %u after %u (sd ls DIR SKIP)\r\n" % (path, len(sd.files), shown, skip))
        elif sub == "save":
            sd.result = self.sd_save(argv[2] if len(argv) >= 3 else "manual")
            r.add("[sd] %s\r\n" % sd.result)
        elif sub == "auto" and len(argv) >= 3:
            self.sd_auto = argv[2] == "on"
            self.store.kv_set("db_sd_auto", "1" if self.sd_auto else "0")
            r.add("[sd] auto-save on ring/motion %s\r\n" % ("on" if self.sd_auto else "off"))
        elif sub == "keep" and len(argv) >= 3:
            self.sd_keep_s = max(5, int(argv[2]) if argv[2].isdigit() else 5)
            self.store.kv_set("db_sd_keep", str(self.sd_keep_s))
            r.add("[sd] keep %u s after the last save\r\n" % self.sd_keep_s)
        else:
            r.add("sd status | mount | umount [off] | format [TOKEN] | ls [DIR] [SKIP] | save [EVENT] | auto on|off | "
                  "pwr PIN|none | pins 1|0 | power on|off | ntp HOST | tz MIN | time EPOCH | sync\r\n")

    # ---- sim controls
    def cmd_sim(self, argv: list[str], r: Reply) -> None:
        sub = argv[1] if len(argv) >= 2 else "help"
        arg = argv[2] if len(argv) >= 3 else ""
        if sub == "press":
            if self.sleeping:
                self.wake("button")
                r.add("[sim] button pressed while asleep: waking (reason button)\r\n")
            else:
                before = self.ring_count
                self.press()
                r.add("[sim] button pressed: ring_count %u%s\r\n" % (self.ring_count, "" if self.ring_count != before else " (debounced, 0.8 s)"))
        elif sub == "motion":
            if self.sleeping:
                if self.pwr_wake_pir:
                    self.wake("pir")
                    r.add("[sim] PIR edge while asleep: waking (reason pir)\r\n")
                else:
                    r.add("[sim] PIR edge while asleep ignored (pwr wake_pir off)\r\n")
            else:
                r.add(self.motion_text(self.pir_trigger()).replace("simulated edge", "PIR edge (GPIO 24)"))
        elif sub == "light" and arg.isdigit():
            self.light_mv = int(arg)
            self.store.set("env", "light_mv", self.light_mv)
            r.add("[sim] light sensor %u mV (%s, dark above %u)\r\n" % (self.light_mv, "dark" if self.light_mv > self.dark_mv else "day", self.dark_mv))
        elif sub == "batt" and arg.isdigit():
            self.batt_mv = int(arg)
            self.store.set("env", "batt_mv", self.batt_mv)
            r.add("[sim] battery %u mV = %u%%\r\n" % (self.batt_mv, self.batt_pct(self.batt_mv, bool(self.charging))))
        elif sub == "usb" and arg in ("on", "off"):
            was = self.usb
            self.usb = 1 if arg == "on" else 0
            self.store.set("env", "usb", self.usb)
            if self.usb and not was and self.sleeping:
                self.wake("usb")
            if not self.usb:
                self.charging = 0
                self.store.set("env", "charging", 0)
            r.add("[sim] usb %d charging %d%s\r\n" % (self.usb, self.charging, " (waking, reason usb)" if self.usb and not was and self.sleeping else ""))
        elif sub == "charging" and arg in ("on", "off"):
            self.charging = 1 if arg == "on" and self.usb else 0
            self.store.set("env", "charging", self.charging)
            r.add("[sim] charging %d%s\r\n" % (self.charging, "" if self.usb or arg == "off" else " (needs usb on)"))
        elif sub == "time" and arg:
            try:
                speed = float(arg)
            except ValueError:
                speed = 0
            if speed <= 0:
                r.add("[sim] time SPEED (> 0; 1 = real time)\r\n")
                return
            self.clock.set_speed(speed)
            r.add("[sim] time x%g\r\n" % speed)
        elif sub == "heartbeat":
            self.last_hb_ms = -10 ** 9
            r.add("[sim] heartbeat report queued\r\n")
        elif sub == "wake":
            if self.sleeping:
                self.wake(arg or "rtc")
                r.add("[sim] waking (reason %s)\r\n" % (arg or "rtc"))
            else:
                r.add("[sim] not asleep\r\n")
        elif sub == "status":
            last = self.reports[-1] if self.reports else None
            r.add("[sim] %s fw %s up %u s speed x%g | light %u mV batt %u mV usb %d chg %d | cam %d frames %u | ring %u motion %u"
                  " | hook %s | last report %s -> %s | ports tcp %d http %d ctl %d rtsp %d%s | %s frames\r\n" % (
                      self.state, self.fw, self.uptime_ms() // 1000, self.clock.speed, self.light_mv, self.batt_mv, self.usb,
                      self.charging, int(self.cam.open), self.cam.frames, self.ring_count, self.motion_count, self.hook or "not set",
                      last["json"]["event"] if last else "-", last["status"] if last else "-", self.args.tcp_port,
                      self.args.http_port, self.args.sim_port, self.rtsp_port, " (ffmpeg)" if self.rtsp.running() else "",
                      "Pillow" if HAVE_PIL else "mosaic"))
        else:
            r.add("sim press | motion | light MV | batt MV | usb on|off | charging on|off | time SPEED | heartbeat | wake [REASON]"
                  " | status | help   (curl http://IP:%d/sim/press)\r\n" % self.args.http_port)

    COMMANDS: dict[str, Callable[["Device", list, Reply], None]] = {
        "pin": cmd_pin, "adc": cmd_adc, "mem": cmd_mem, "reboot": cmd_reboot,
        "wifi": cmd_wifi, "hook": cmd_hook, "pir": cmd_pir, "motion": cmd_motion, "ota": cmd_ota, "tok": cmd_tok,
        "led": cmd_led, "night": cmd_night, "sys": cmd_sys, "ring": cmd_ring, "pwr": cmd_pwr,
        "cam": cmd_cam, "rtsp": cmd_rtsp, "spk": cmd_spk, "mic": cmd_mic,
        "talk": cmd_talk, "sd": cmd_sd,          # feat/talk, feat/sd
        "vol": cmd_vol, "log": cmd_log,          # fw 0.25 (policy.c)
        "sim": cmd_sim,                            # simulator only
    }


# ---------------------------------------------------------------- audio helpers
def synth_pcm16(rate: int, seconds: float, hz: float, amp: int = 6000) -> bytes:
    n = int(rate * seconds)
    step = 2 * math.pi * hz / rate
    return struct.pack("<%dh" % n, *(int(amp * math.sin(i * step) + random.randint(-150, 150)) for i in range(n)))


def wav_header(rate: int, data_len: int, channels: int = 1) -> bytes:
    return struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", (36 + data_len) & 0xFFFFFFFF, b"WAVE", b"fmt ", 16, 1, channels,
                       rate, rate * channels * 2, channels * 2, 16, b"data", data_len & 0xFFFFFFFF)


def parse_wav(body: bytes) -> Optional[tuple[int, int, int, int]]:
    """(rate, channels, bits, data offset) for a PCM WAV, None if not a WAV."""
    if len(body) < 12 or body[:4] != b"RIFF" or body[8:12] != b"WAVE":
        return None
    off, fmt = 12, None
    while off + 8 <= len(body):
        cid, ln = body[off:off + 4], struct.unpack("<I", body[off + 4:off + 8])[0]
        if cid == b"fmt " and off + 24 <= len(body):
            fmt = struct.unpack("<HHIIHH", body[off + 8:off + 24])
        elif cid == b"data":
            if fmt is None:
                return None
            return fmt[2], fmt[1], fmt[5], off + 8
        off += 8 + ln + (ln & 1)
    return None


def play_tone_on_host(hz: int, ms: int) -> None:
    """--audio: best effort playback through the host (afplay / aplay / ffplay)."""
    play_pcm_on_host(synth_pcm16(SPK_RATE, ms / 1000, hz), SPK_RATE)


def play_pcm_on_host(pcm: bytes, rate: int) -> None:
    player = next((p for p in ("afplay", "aplay", "paplay", "ffplay") if shutil.which(p)), None)
    if not player or not pcm:
        return
    path = Path(os.environ.get("TMPDIR", "/tmp")) / ("vdb_speak_%d.wav" % os.getpid())
    path.write_bytes(wav_header(rate, len(pcm)) + pcm)
    cmd = [player, str(path)] if player != "ffplay" else [player, "-nodisp", "-autoexit", "-loglevel", "quiet", str(path)]
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ---------------------------------------------------------------- RTSP stand-in (ffmpeg)
class RtspStandIn:
    """`rtsp on` with --rtsp and ffmpeg present: ffmpeg serves one RTSP client at rtsp://HOST:PORT/live
    (testsrc H.264 + a PCMA/8000 sine as the microphone track, like the firmware's SDP; no backchannel).
    Without ffmpeg the server is "stopped" and `rtsp status` says why."""

    def __init__(self, dev: Device) -> None:
        self.dev = dev
        self.proc: Optional[subprocess.Popen] = None
        self.port = 0
        self.started_ms = 0

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def playing(self) -> int:
        return 0   # ffmpeg does not tell us; the camera therefore never flips to H.264 on its own

    def start(self, port: int) -> int:
        if self.running():
            return 0
        if not self.dev.args.rtsp:
            log.info("[rtsp] stand-in disabled (start the simulator with --rtsp)")
            return -1
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            log.info("[rtsp] ffmpeg not found: no RTSP stand-in")
            return -1
        cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi",
               "-i", "testsrc=size=1280x720:rate=15", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000",
               "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
               "-g", "30", "-b:v", "1500k", "-pix_fmt", "yuv420p", "-c:a", "pcm_alaw", "-ar", "8000", "-ac", "1",
               "-f", "rtsp", "-rtsp_flags", "listen", "rtsp://0.0.0.0:%d/live" % port]
        try:
            self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            log.info("[rtsp] ffmpeg failed: %s", e)
            return -1
        self.port, self.started_ms = port, self.dev.clock.now_ms()
        log.info("[rtsp] server on port %u (/live) via ffmpeg pid %d (one client at a time)", port, self.proc.pid)
        return 0

    def stop(self) -> None:
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            log.info("[rtsp] stopped")
        self.proc = None

    def status(self) -> str:
        run = self.running()
        frames = (self.dev.clock.now_ms() - self.started_ms) * 15 // 1000 if run else 0
        return ("[rtsp] %s port %u clients %d/%d (%d playing) in %u.%u fps %u kbps frames %u key %u drop busy 0 big 0 sps %u pps %u age %u ms\r\n"
                % ("running" if run else "stopped", self.port or self.dev.rtsp_port, 0, RTSP_MAX_CLIENTS, 0, 15 if run else 0, 0,
                   1500 if run else 0, frames, frames // 30, 26 if run else 0, 4 if run else 0, 0 if run else 0))


# ---------------------------------------------------------------- network front-ends
class CamHttpServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def server_bind(self) -> None:   # skip HTTPServer's getfqdn(): a slow reverse DNS stalled the boot on macOS
        socket.socket.bind(self.socket, self.server_address)
        self.server_address = self.socket.getsockname()
        self.server_name, self.server_port = self.server_address[0], self.server_address[1]


class Network:
    """Owns the listeners that exist only while the device is "up": TCP 2323 and HTTP 8080."""

    def __init__(self, dev: Device) -> None:
        self.dev = dev
        self.tcp: Optional[socket.socket] = None
        self.http: Optional[ThreadingHTTPServer] = None
        self.generation = 0
        self._lock = threading.Lock()

    def up(self) -> None:
        with self._lock:
            self.generation += 1
            gen = self.generation
            self.tcp = _listen(self.dev.args.bind, self.dev.args.tcp_port, backlog=1)
            threading.Thread(target=self._tcp_loop, args=(self.tcp, gen), name="tcpcmd", daemon=True).start()
            self.http = CamHttpServer((self.dev.args.bind, self.dev.args.http_port), HttpHandler)
            self.http.device = self.dev  # type: ignore[attr-defined]
            threading.Thread(target=self.http.serve_forever, kwargs={"poll_interval": 0.1}, name="camhttp", daemon=True).start()

    def down(self) -> None:
        with self._lock:
            self.generation += 1
            if self.tcp:
                try:
                    self.tcp.close()
                except OSError:
                    pass
                self.tcp = None
            if self.http:
                http = self.http
                self.http = None
                threading.Thread(target=http.shutdown, daemon=True).start()
                try:
                    http.server_close()
                except OSError:
                    pass
            for s in list(self.dev._http_sockets):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            self.dev.rtsp.stop()
            log.info("[net] ports %d/%d closed", self.dev.args.tcp_port, self.dev.args.http_port)

    def _tcp_loop(self, ls: socket.socket, gen: int) -> None:
        while self.generation == gen:
            try:
                cs, peer = ls.accept()
            except OSError:
                return
            try:
                handle_command_connection(self.dev, cs, peer, control=False)
            except Exception:  # pragma: no cover
                log.exception("tcp client")
            finally:
                cs.close()


def _listen(bind: str, port: int, backlog: int) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind, port))
    s.listen(backlog)
    return s


def handle_command_connection(dev: Device, cs: socket.socket, peer, control: bool) -> None:
    """tcp_cmd_task(): one recv, `<token> <command>`, reply, 100 ms, close."""
    cs.settimeout(5.0)
    try:
        raw = cs.recv(TCP_LINE_MAX)
    except OSError:
        return
    if not raw:
        return
    line = raw.decode(errors="replace")
    sp = line.find(" ")
    token = dev.token
    if (not token and not control) or sp < 0 or line[:sp] != (token if token else line[:sp]):
        log.info("[%s] %s denied", "ctl" if control else "tcp", peer[0])
        _send(cs, b"denied\n")
    else:
        cmd = line[sp + 1:].strip()
        log.info("[%s] %s", "ctl" if control else "tcp", cmd)
        if not control:
            dev.activity()
        status, text = dev.run_line(cmd, allow={"sim"} if control else None)
        _send(cs, (status + "\n").encode())
        if text:
            _send(cs, text.encode())
    time.sleep(0.1)


def _send(cs: socket.socket, data: bytes) -> None:
    try:
        cs.sendall(data)
    except OSError:
        pass


def control_loop(dev: Device, ls: socket.socket) -> None:
    """The always-on `sim` control port (survives sleep and reboot)."""
    while not dev.stop_all.is_set():
        try:
            cs, peer = ls.accept()
        except OSError:
            return
        try:
            handle_command_connection(dev, cs, peer, control=True)
        finally:
            cs.close()


def stdin_loop(dev: Device) -> None:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if not line.startswith("sim"):
            line = "sim " + line
        status, text = dev.run_line(line)
        sys.stdout.write("%s\n%s" % (status, text.replace("\r\n", "\n")))
        sys.stdout.flush()


# ---------------------------------------------------------------- HTTP (cam_http_client_task + talk_http_handle)
class HttpHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: ThreadingHTTPServer

    def log_message(self, fmt: str, *args) -> None:   # one line per request, our format
        pass

    @property
    def dev(self) -> Device:
        return self.server.device  # type: ignore[attr-defined]

    def _raw(self, code: int, text: str, ctype: Optional[str], body: bytes, extra: str = "") -> None:
        hdr = "HTTP/1.1 %d %s\r\n" % (code, text)
        if ctype:
            hdr += "Content-Type: %s\r\n" % ctype
        hdr += "Content-Length: %d\r\n%sConnection: close\r\n\r\n" % (len(body), extra)
        self.wfile.write(hdr.encode() + body)
        self.wfile.flush()
        self.close_connection = True
        log.info("[http] %s %s -> %d %d B", self.command, self.path, code, len(body))

    def _talk_reply(self, code: int, ctype: str, body: str) -> None:
        text = {200: "OK", 400: "Bad Request", 404: "Not Found", 411: "Length Required", 415: "Unsupported Media Type",
                431: "Request Header Fields Too Large", 502: "Bad Gateway", 503: "Service Unavailable"}.get(code, "Error")
        self.dev.talk.last_err = code
        self._raw(code, text, ctype, body.encode(), "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n")

    def do_POST(self) -> None:
        self.do_GET()

    def do_GET(self) -> None:
        dev = self.dev
        dev.http_clients += 1
        dev._http_sockets.add(self.connection)
        dev.activity()
        try:
            self._route()
        except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError):
            pass
        finally:
            dev.http_clients -= 1
            dev._http_sockets.discard(self.connection)

    def _route(self) -> None:
        dev, path = self.dev, self.path
        if path.startswith("/sim/") or path == "/sim":
            return self._sim()
        if path.startswith(("/speak", "/mic.wav", "/talk")):
            return self._talk()
        if self.command == "GET" and re.match(r"^/(cmd|log)(\?|$)", path):
            return self._cmd()
        if self.command != "GET":
            return self._raw(400, "Bad Request", "text/plain", b"GET only\n")
        if path.startswith("/stream") or path.startswith("/snapshot"):
            if dev.cam_want_jpeg() == 0:
                deadline = time.monotonic() + 3.0 / dev.clock.speed + 0.5   # the firmware waits up to 3 s for the first frame
                while not dev.cam.jpeg and time.monotonic() < deadline:
                    time.sleep(0.01)
            elif path.startswith("/stream") or not dev.cam.jpeg:
                return self._raw(503, "Service Unavailable", None, b"camera busy (H.264/RTSP)")
        if not dev.cam.ever_opened:
            return self._raw(503, "Service Unavailable", None, b"camera not open")
        if path.startswith("/stream"):
            return self._stream()
        if path.startswith("/snapshot"):
            jpeg = dev.cam.jpeg
            if jpeg:
                return self._raw(200, "OK", "image/jpeg", jpeg, "Cache-Control: no-cache\r\n")
            return self._raw(503, "Service Unavailable", None, b"no frame yet")
        c = dev.cam
        body = ("<html><body>doorbell %s cam open=%d frames=%u %ux%u<br>"
                "<a href=/snapshot.jpg>snapshot.jpg</a> <a href=/stream>stream</a></body></html>" % (dev.fw, int(c.open), c.frames, c.w, c.h))
        self._raw(200, "OK", "text/html", body.encode())

    def _stream(self) -> None:
        dev = self.dev
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                         b"Cache-Control: no-cache\r\nConnection: close\r\n\r\n")
        log.info("[http] GET %s -> 200 mjpeg", self.path)
        last, gen, sent = 0, dev.net.generation, 0
        try:
            while dev.net.generation == gen and not dev.stop_all.is_set():
                deadline = time.monotonic() + 2.0
                while dev.cam.seq == last and time.monotonic() < deadline:
                    time.sleep(0.01)
                if dev.cam.seq == last:
                    if not dev.cam.open:
                        break
                    continue
                jpeg, last = dev.cam.jpeg, dev.cam.seq
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg) + jpeg + b"\r\n")
                self.wfile.flush()
                sent += 1
        finally:
            self.close_connection = True
            log.info("[http] stream closed after %u frames", sent)

    def _cmd(self) -> None:
        """policy_http_handle(): GET /cmd?t=TOKEN&c=COMMAND -> the dispatcher's REPLY text; GET /log?t=TOKEN -> the ring."""
        dev = self.dev
        q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(self.path).query, keep_blank_values=True))
        if not dev.token or q.get("t") != dev.token:
            log.info("[http] %s: denied", self.path[:4])
            return self._raw(403, "Forbidden", "text/plain", b"denied\n", "Cache-Control: no-cache\r\n")
        if self.path.startswith("/log"):
            return self._raw(200, "OK", "text/plain", ramlog.all(), "Cache-Control: no-cache\r\n")
        cmd = q.get("c", "").strip()
        if not cmd:
            return self._raw(400, "Bad Request", "text/plain", b"c=<command> missing\n", "Cache-Control: no-cache\r\n")
        log.info("[http] cmd: %s", cmd)
        dev.activity()
        status, text = dev.run_line(cmd[:159])
        if status != "ok":
            return self._raw(404, "Not Found", "text/plain", b"unknown command\n", "Cache-Control: no-cache\r\n")
        self._raw(200, "OK", "text/plain", text.encode(), "Cache-Control: no-cache\r\n")

    def _sim(self) -> None:
        rest = urllib.parse.unquote(self.path[len("/sim"):]).strip("/")
        q = rest.split("?", 1)
        argv = ["sim"] + [a for a in re.split(r"[/+ ]", q[0]) if a]
        if len(q) > 1:
            argv += [v for _, v in urllib.parse.parse_qsl(q[1])]
        log.info("[http] %s %s", self.command, self.path)
        status, text = self.dev.run_line(" ".join(argv))
        code = 200 if text and not text.startswith("sim press |") else 404
        self._raw(code, "OK" if code == 200 else "Not Found", "text/plain", text.replace("\r\n", "\n").encode())

    # ---- feat/talk: /speak, /mic.wav, /talk
    def _talk(self) -> None:
        dev, t = self.dev, self.dev.talk
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        if url.path.startswith("/talk"):
            return self._talk_reply(200, "text/html", TALK_PAGE)
        if url.path.startswith("/mic.wav"):
            return self._mic(int(query.get("seconds", "0") or 0))
        if self.command == "POST":
            return self._speak_post()
        if "url" in query:
            return self._speak_url(query["url"])
        if "tone" in query:
            hz = int(query["tone"]) if query["tone"].isdigit() else 0
            ms = int(query.get("ms", "500")) if query.get("ms", "500").isdigit() else 500
            return self._speak_tone(hz, ms)
        self._talk_reply(400, "text/plain", "POST /speak (raw s16le mono 8 kHz or WAV) | GET /speak?tone=HZ&ms=MS | "
                                            "GET /speak?url=http://IP[:port]/file.wav | GET /mic.wav[?seconds=N] | GET /talk\n")

    def _speak_reply(self, status: int, ms: int, in_bytes: int, in_rate: int, what: str) -> None:
        if status == 200:
            body = '{"ok":true,"source":"%s","ms":%u,"in_bytes":%u,"in_rate":%u,"dma_frames":%u,"underruns":0}\n' % (
                what, ms, in_bytes, in_rate, ms * SPK_RATE // 1000 // 320)
        else:
            hint = {415: "need PCM 16-bit WAV (>= 8 kHz, 1-2 ch) or raw s16le mono 8 kHz", 503: "speaker busy",
                    502: "fetch failed"}.get(status, "bad request")
            body = '{"ok":false,"source":"%s","status":%d,"ms":%u,"in_bytes":%u,"hint":"%s"}\n' % (what, status, ms, in_bytes, hint)
        self._talk_reply(status, "application/json", body)

    def _speak_open(self, what: str) -> bool:
        dev = self.dev
        if dev.spk_busy or dev.talk.speaking:
            self._speak_reply(503, 0, 0, 0, what)
            return False
        dev.talk.speaking, dev.talk.abort, dev.spk_busy = True, False, True
        dev.talk.speak_calls += 1
        log.info("[talk] speak (%s): PA on, gain 0x%x", what, dev.talk_gain or dev.spk_gain())
        return True

    def _speak_close(self, ms: int, in_bytes: int, in_rate: int) -> None:
        t = self.dev.talk
        self.dev.spk_busy = t.speaking = False
        t.speak_last_ms, t.speak_last_rate = ms, in_rate
        t.speak_in_bytes += in_bytes

    def _play(self, pcm: bytes, rate: int) -> int:
        """'Play' PCM: takes real time (scaled), honours talk stop / max; returns ms played."""
        dev = self.dev
        total_ms = len(pcm) * 1000 // (rate * 2)
        ms = min(total_ms, dev.talk_max_s * 1000)
        if dev.args.audio:
            play_pcm_on_host(pcm[: ms * rate * 2 // 1000], rate)
        t0 = dev.clock.now_ms()
        while dev.clock.now_ms() - t0 < ms and not dev.talk.abort and not dev.stop_all.is_set():
            time.sleep(0.01)
        return min(ms, dev.clock.now_ms() - t0)

    def _speak_tone(self, hz: int, ms: int) -> None:
        if hz < 50 or hz > SPK_RATE // 2:
            hz = 1000
        if ms < 10:
            ms = 500
        ms = min(ms, self.dev.talk_max_s * 1000)
        if not self._speak_open("tone"):
            return
        played = self._play(synth_pcm16(SPK_RATE, ms / 1000, hz, 8191), SPK_RATE)
        self._speak_close(played, 0, SPK_RATE)
        log.info("[talk] tone %u Hz: %u samples", hz, played * SPK_RATE // 1000)
        self._speak_reply(200, played, 0, SPK_RATE, "tone")

    def _read_body(self) -> Optional[bytes]:
        te = self.headers.get("Transfer-Encoding", "")
        if "chunked" in te.lower():
            out = bytearray()
            while True:
                size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    return bytes(out)
                out += self.rfile.read(size)
                self.rfile.readline()
        cl = self.headers.get("Content-Length")
        if cl is None:
            return None if self.request_version != "HTTP/1.0" else self.rfile.read()
        return self.rfile.read(int(cl))

    def _speak_post(self) -> None:
        body = self._read_body()
        if body is None:
            return self._talk_reply(411, "text/plain", "Content-Length or chunked body required\n")
        if not self._speak_open("post"):
            return
        status, ms, rate = 200, 0, SPK_RATE
        wav = parse_wav(body)
        if wav:
            rate, channels, bits, off = wav
            if bits != 16 or rate < 8000 or channels not in (1, 2):
                status = 415
            else:
                pcm = body[off:]
                if channels == 2:
                    pcm = b"".join(pcm[i:i + 2] for i in range(0, len(pcm) - 3, 4))
                ms = self._play(pcm, rate)
        else:
            ms = self._play(body, SPK_RATE)
        self._speak_close(ms, len(body), rate)
        log.info("[talk] speak: %u body bytes (%s, %u Hz) -> %u ms", len(body), "wav" if wav else "raw", rate, ms)
        self._speak_reply(status, ms, len(body), rate, "post")

    def _speak_url(self, url: str) -> None:
        if not url.startswith("http://"):
            return self._talk_reply(400, "text/plain", "url must be http://IP[:port]/path\n")
        if not self._speak_open("url"):
            return
        status, ms, rate, body = 200, 0, SPK_RATE, b""
        try:
            with urllib.request.urlopen(url, timeout=10) as resp:
                body = resp.read(4 * 1024 * 1024)
        except Exception as e:
            log.info("[talk] fetch %s failed: %s", url, e)
            status = 502
        if status == 200:
            wav = parse_wav(body)
            if wav and wav[2] == 16:
                rate = wav[0]
                ms = self._play(body[wav[3]:], rate)
            elif wav:
                status = 415
            else:
                ms = self._play(body, SPK_RATE)
        self._speak_close(ms, len(body), rate)
        self._speak_reply(status, ms, len(body), rate, "url")

    def _mic(self, seconds: int) -> None:
        dev, t = self.dev, self.dev.talk
        if t.listening or dev.mic_busy:
            return self._talk_reply(503, "text/plain", "microphone busy\n")
        seconds = min(seconds, TALK_MIC_MAX_S)
        chunked = self.request_version != "HTTP/1.0" and seconds == 0
        limit = seconds * TALK_MIC_RATE * 2 if seconds else 0xFFFFFFFF
        t.listening, t.abort, dev.mic_busy = True, False, True
        t.mic_calls += 1
        t.last_err = 200
        common = "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n"
        if seconds:
            hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %u\r\n%s" % (44 + limit, common)
        else:
            hdr = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n%s%s" % ("Transfer-Encoding: chunked\r\n" if chunked else "", common)
        log.info("[http] GET %s -> 200 audio/wav (%s)", self.path, "%u s" % seconds if seconds else "until closed")

        def piece(data: bytes) -> None:
            if chunked:
                self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
            else:
                self.wfile.write(data)
            self.wfile.flush()

        sent, phase = 0, 0
        try:
            self.wfile.write(hdr.encode())
            piece(wav_header(TALK_MIC_RATE, limit if seconds else 0xFFFFFFFF - 44))
            step = TALK_MIC_RATE // 10   # 100 ms pieces, paced at (scaled) real time
            while sent < limit and not t.abort and not dev.stop_all.is_set():
                n = min(step, (limit - sent) // 2)
                pcm = struct.pack("<%dh" % n, *(int(2500 * math.sin((phase + i) * 2 * math.pi * 440 / TALK_MIC_RATE))
                                                 + random.randint(-120, 120) for i in range(n)))
                phase += n
                piece(pcm)
                sent += len(pcm)
                time.sleep(0.1 / dev.clock.speed)
            if chunked:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
        finally:
            t.listening = dev.mic_busy = False
            t.mic_out_bytes += sent
            self.close_connection = True
            log.info("[talk] mic: %u bytes out", sent)


TALK_PAGE = ("<!doctype html><html><head><meta name=viewport content='width=device-width'><title>doorbell talk</title></head>"
             "<body style='font-family:sans-serif'><h3>Virtual doorbell: talk</h3>"
             "<button onclick=\"go('/speak?tone=1000&ms=500')\">tone 1 kHz</button> "
             "<button onclick=\"go('/speak?tone=440&ms=1500')\">tone 440 Hz</button> "
             "<input id=f type=file accept='audio/wav'> <button onclick='send()'>POST /speak</button><br><br>"
             "<audio controls src='/mic.wav?seconds=5'></audio> (mic, 5 s)<pre id=o></pre>"
             "<script>const o=document.getElementById('o'),f=document.getElementById('f');"
             "function show(r){r.text().then(t=>o.textContent=r.status+' '+t)}"
             "function go(p){o.textContent='...';fetch(p).then(show)}"
             "function send(){if(!f.files[0])return;o.textContent='sending';"
             "fetch('/speak',{method:'POST',body:f.files[0]}).then(show)}</script></body></html>")


# ---------------------------------------------------------------- main
def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="virtual doorbell-bk7258 device", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--tcp-port", type=int, default=int(os.environ.get("VDB_TCP_PORT", TCP_CMD_PORT)), help="TCP command channel")
    ap.add_argument("--http-port", type=int, default=int(os.environ.get("VDB_HTTP_PORT", CAM_HTTP_PORT)), help="camera/talk HTTP server")
    ap.add_argument("--rtsp-port", type=int, default=8554, help="RTSP stand-in port (firmware default is 554)")
    ap.add_argument("--sim-port", type=int, default=None, help="always-on `sim` control port (default: tcp-port + 1)")
    ap.add_argument("--bind", default="0.0.0.0", help="listen address")
    ap.add_argument("--token", default=None, help="TCP channel token (else env DOORBELL_TOKEN, else the stored one, else 'sim')")
    ap.add_argument("--state", default=os.environ.get("VDB_STATE", "~/.virtual_doorbell.json"), help="KV + sensor state file")
    ap.add_argument("--hook", default=None, help="webhook HOST:PORT/PATH (overrides the stored db_hook)")
    ap.add_argument("--fps", type=float, default=5.0, help="camera frame rate")
    ap.add_argument("--fw-version", default=None, help="reported firmware version (default: FW_VERSION from app_main.c)")
    ap.add_argument("--ip", default=None, help="IP reported in JSON / sys info (default: the host's LAN address)")
    ap.add_argument("--rtsp", action="store_true", help="serve rtsp://HOST:RTSP_PORT/live with ffmpeg on `rtsp on`")
    ap.add_argument("--audio", action="store_true", help="play /speak and spk tones through the host speaker")
    ap.add_argument("--time", type=float, default=1.0, help="initial time speed (like `sim time`)")
    ap.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    args = ap.parse_args(argv)
    args.token_from_cli = args.token is not None or "DOORBELL_TOKEN" in os.environ
    args.token = args.token or os.environ.get("DOORBELL_TOKEN") or "sim"
    args.sim_port = args.sim_port or args.tcp_port + 1
    args.ip = args.ip or _lan_ip()
    return args


def _lan_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))   # no traffic; just to pick the outbound interface
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.log_level), stream=sys.stdout,
                        format="%(asctime)s.%(msecs)03d %(message)s", datefmt="%H:%M:%S")
    clock = SimClock(args.time)
    store = Store(Path(args.state).expanduser())
    dev = Device(args, store, clock)
    ctl = _listen(args.bind, args.sim_port, backlog=4)
    threading.Thread(target=control_loop, args=(dev, ctl), name="ctl", daemon=True).start()
    threading.Thread(target=dev.reporter_loop, name="reporter", daemon=True).start()
    if sys.stdin.isatty() or os.environ.get("VDB_STDIN") == "1":   # interactive `sim` commands on stdin
        threading.Thread(target=stdin_loop, args=(dev,), name="stdin", daemon=True).start()
    log.info("virtual doorbell fw %s: tcp %d http %d ctl %d | token '%s' | state %s | hook %s | frames: %s%s",
             dev.fw, args.tcp_port, args.http_port, args.sim_port, dev.token, store.path, dev.hook or "not set",
             "Pillow" if HAVE_PIL else "pure-python mosaic", " | rtsp stand-in on %d" % args.rtsp_port if args.rtsp else "")
    log.info("try: DOORBELL_HOST=127.0.0.1 DOORBELL_TOKEN=%s python3 tools/dbcmd.py --port %d sys info   |   curl http://127.0.0.1:%d/sim/press",
             dev.token, args.tcp_port, args.http_port)
    dev.boot("power-on", None)

    def on_signal(signum, frame) -> None:
        log.info("signal %d: shutting down", signum)
        dev.stop_all.set()

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    try:
        while not dev.stop_all.is_set():
            time.sleep(0.2)
    finally:
        dev.net.down()
        ctl.close()
        dev.rtsp.stop()
        store.save()
    return 0


if __name__ == "__main__":
    sys.exit(main())
