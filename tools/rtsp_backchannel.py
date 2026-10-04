#!/usr/bin/env python3
"""RTSP ONVIF-backchannel test client for the doorbell (fw 0.26+): talk into the speaker and/or
record the microphone track, the way go2rtc does it, with nothing but the standard library.

  python3 tools/rtsp_backchannel.py rtsp://<device-ip>:554/live --tone 1000 --seconds 3 --record mic.wav
  python3 tools/rtsp_backchannel.py rtsp://<device-ip>:554/live --send hello_8k.wav
  python3 tools/rtsp_backchannel.py rtsp://<device-ip>:554/live --record mic.wav --seconds 10   # listen only
  python3 tools/rtsp_backchannel.py rtsp://<device-ip>:554/live --describe                       # print the SDP

What it does: OPTIONS, DESCRIBE with `Require: www.onvif.org/ver20/backchannel`, SETUP of the
microphone track (`a=recvonly`, server -> client) and of the backchannel track (`a=sendonly`,
client -> server) as RTP/AVP/TCP interleaved, PLAY, then it sends 20 ms G.711 frames (PT 8 PCMA
or 0 PCMU, whatever the SDP offers) paced in real time while it writes every mic RTP packet it
receives into a WAV (8 kHz mono s16le). The video track is not set up (the server keeps the
camera in H.264 mode anyway). TEARDOWN at the end.

--analyze FREQ prints the Goertzel level of FREQ and the broadband rms in the recorded WAV, in dB
relative to full scale, so an echo test is one command:
  talk aec off -> --tone 1000 --seconds 3 --record off.wav --analyze 1000
  talk aec on  -> --tone 1000 --seconds 3 --record on.wav  --analyze 1000
and the attenuation is the difference of the two "@1000 Hz" figures.
"""
from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import time
import wave
from urllib.parse import urlparse

ONVIF_REQUIRE = "www.onvif.org/ver20/backchannel"

# ---------------------------------------------------------------- G.711 (ITU-T), table-free reference code


def _alaw_encode_sample(s: int) -> int:
    s = max(-32768, min(32767, s))
    sign = 0x80 if s >= 0 else 0
    if s < 0:
        s = -s - 1
    s >>= 3                       # 13-bit magnitude
    if s >= 16:
        seg = 1
        while s >= 32 and seg < 7:
            s >>= 1
            seg += 1
        a = (seg << 4) | (s & 0x0F)
    else:
        a = s
    return (a ^ 0x55) ^ sign


def _alaw_decode_sample(a: int) -> int:
    a ^= 0x55
    sign = a & 0x80
    seg = (a >> 4) & 7
    mant = a & 0x0F
    if seg == 0:
        v = (mant << 4) + 8
    else:
        v = ((mant << 4) + 0x108) << (seg - 1)
    return v if sign else -v


def _ulaw_encode_sample(s: int) -> int:
    BIAS, CLIP = 0x84, 32635
    s = max(-32768, min(32767, s))
    sign = 0x80 if s < 0 else 0
    if s < 0:
        s = -s
    s = min(s, CLIP) + BIAS
    seg = 7
    while seg > 0 and not (s & (0x80 << seg)):
        seg -= 1
    mant = (s >> (seg + 3)) & 0x0F
    return ~(sign | (seg << 4) | mant) & 0xFF


def _ulaw_decode_sample(u: int) -> int:
    u = ~u & 0xFF
    sign, seg, mant = u & 0x80, (u >> 4) & 7, u & 0x0F
    v = (((mant << 3) + 0x84) << seg) - 0x84
    return -v if sign else v


ALAW_ENC = bytes(_alaw_encode_sample(s) for s in range(-32768, 32768))   # index: s + 32768
ULAW_ENC = bytes(_ulaw_encode_sample(s) for s in range(-32768, 32768))
ALAW_DEC = [_alaw_decode_sample(a) for a in range(256)]
ULAW_DEC = [_ulaw_decode_sample(u) for u in range(256)]


def g711_encode(pcm: bytes, pt: int) -> bytes:
    tbl = ALAW_ENC if pt == 8 else ULAW_ENC
    return bytes(tbl[s + 32768] for s in struct.unpack("<%dh" % (len(pcm) // 2), pcm))


def g711_decode(data: bytes, pt: int) -> bytes:
    tbl = ALAW_DEC if pt == 8 else ULAW_DEC
    return struct.pack("<%dh" % len(data), *(tbl[b] for b in data))


# ---------------------------------------------------------------- audio sources


def wav_to_pcm8k(path: str) -> bytes:
    """Any PCM-16 WAV -> s16le mono 8 kHz (box-average decimation like the firmware does)."""
    w = wave.open(path)
    rate, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw != 2:
        sys.exit("need a 16-bit PCM WAV")
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    if ch > 1:
        samples = samples[::ch]
    if rate == 8000:
        return struct.pack("<%dh" % len(samples), *samples)
    out, acc, acc_n, pos, step = [], 0, 0, 0, rate / 8000.0
    for s in samples:
        acc += s
        acc_n += 1
        pos += 1
        if pos >= step:
            pos -= step
            out.append(int(acc / acc_n))
            acc = acc_n = 0
    return struct.pack("<%dh" % len(out), *out)


def tone_pcm8k(hz: float, seconds: float, amp: int = 8191) -> bytes:
    n = int(seconds * 8000)
    return struct.pack("<%dh" % n, *(int(amp * math.sin(2 * math.pi * hz * i / 8000)) for i in range(n)))


# ---------------------------------------------------------------- RTSP client


class Rtsp:
    def __init__(self, url: str, timeout: float = 8.0, verbose: bool = False) -> None:
        u = urlparse(url)
        self.url, self.host, self.port = url, u.hostname, u.port or 554
        self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
        self.cseq, self.session, self.base = 0, "", url if url.endswith("/") else url + "/"
        self.buf = b""
        self.verbose = verbose

    def request(self, method: str, url: str, headers: dict | None = None) -> tuple[int, dict, bytes]:
        self.cseq += 1
        lines = ["%s %s RTSP/1.0" % (method, url), "CSeq: %d" % self.cseq, "User-Agent: doorbell-rtsp_backchannel"]
        if self.session:
            lines.append("Session: %s" % self.session)
        for k, v in (headers or {}).items():
            lines.append("%s: %s" % (k, v))
        req = ("\r\n".join(lines) + "\r\n\r\n").encode()
        if self.verbose:
            print(">>", req.decode().strip().replace("\r\n", "\n   "))
        self.sock.sendall(req)
        return self.read_response()

    def _fill(self) -> bool:
        d = self.sock.recv(4096)
        if not d:
            return False
        self.buf += d
        return True

    def read_response(self) -> tuple[int, dict, bytes]:
        """Next RTSP response; interleaved frames that arrive first are handed to on_frame."""
        while True:
            if self.buf[:1] == b"$":
                if len(self.buf) < 4:
                    if not self._fill():
                        raise ConnectionError("closed")
                    continue
                ch, n = self.buf[1], struct.unpack(">H", self.buf[2:4])[0]
                if len(self.buf) < 4 + n:
                    if not self._fill():
                        raise ConnectionError("closed")
                    continue
                self.on_frame(ch, self.buf[4:4 + n])
                self.buf = self.buf[4 + n:]
                continue
            end = self.buf.find(b"\r\n\r\n")
            if end < 0:
                if not self._fill():
                    raise ConnectionError("closed")
                continue
            head = self.buf[:end].decode(errors="replace")
            first, *hl = head.split("\r\n")
            hdr = {}
            for line in hl:
                k, _, v = line.partition(":")
                hdr[k.strip().lower()] = v.strip()
            cl = int(hdr.get("content-length", "0") or 0)
            if len(self.buf) < end + 4 + cl:
                if not self._fill():
                    raise ConnectionError("closed")
                continue
            body = self.buf[end + 4:end + 4 + cl]
            self.buf = self.buf[end + 4 + cl:]
            if self.verbose:
                print("<<", head.replace("\r\n", "\n   "))
            code = int(first.split()[1])
            if "session" in hdr and not self.session:
                self.session = hdr["session"].split(";")[0]
            if "content-base" in hdr:
                self.base = hdr["content-base"]
            return code, hdr, body

    def on_frame(self, ch: int, data: bytes) -> None:   # overridden by the session
        pass

    def pump(self, max_wait: float) -> None:
        """Receive interleaved frames (and stray responses) for up to max_wait seconds."""
        self.sock.settimeout(max_wait)
        try:
            if not self._fill():
                raise ConnectionError("closed")
        except socket.timeout:
            return
        finally:
            self.sock.settimeout(8.0)
        while True:
            if self.buf[:1] == b"$":
                if len(self.buf) < 4:
                    return
                ch, n = self.buf[1], struct.unpack(">H", self.buf[2:4])[0]
                if len(self.buf) < 4 + n:
                    return
                self.on_frame(ch, self.buf[4:4 + n])
                self.buf = self.buf[4 + n:]
            elif self.buf.startswith(b"RTSP/"):
                end = self.buf.find(b"\r\n\r\n")
                if end < 0:
                    return
                self.buf = self.buf[end + 4:]   # keepalive replies etc.
            elif self.buf:
                i = self.buf.find(b"$")
                self.buf = self.buf[i:] if i >= 0 else b""
                if i < 0:
                    return
            else:
                return

    def control_url(self, control: str) -> str:
        if "://" in control:
            return control
        return self.base + control


def parse_sdp(sdp: str) -> list[dict]:
    medias, cur = [], None
    for line in sdp.splitlines():
        line = line.strip()
        if line.startswith("m="):
            parts = line[2:].split()
            cur = {"kind": parts[0], "proto": parts[2], "pts": [int(p) for p in parts[3:] if p.isdigit()],
                   "control": "", "dir": "", "rtpmap": {}}
            medias.append(cur)
        elif cur is not None and line.startswith("a="):
            a = line[2:]
            if a.startswith("control:"):
                cur["control"] = a[8:]
            elif a in ("sendonly", "recvonly", "sendrecv"):
                cur["dir"] = a
            elif a.startswith("rtpmap:"):
                pt, _, enc = a[7:].partition(" ")
                cur["rtpmap"][int(pt)] = enc
    return medias


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url")
    ap.add_argument("--describe", action="store_true", help="print the SDP (with the Require header) and exit")
    ap.add_argument("--no-require", action="store_true", help="DESCRIBE without the ONVIF Require header")
    ap.add_argument("--send", metavar="WAV", help="play this PCM-16 WAV through the backchannel")
    ap.add_argument("--tone", type=float, metavar="HZ", help="play a sine (-12 dBFS) through the backchannel")
    ap.add_argument("--amp", type=int, default=8191, help="tone amplitude (default 8191 = -12 dBFS)")
    ap.add_argument("--seconds", type=float, default=3.0, help="tone length / listen time (default 3)")
    ap.add_argument("--record", metavar="WAV", help="write the microphone track here (8 kHz mono WAV)")
    ap.add_argument("--analyze", type=float, metavar="HZ", help="after recording: level at HZ and rms, in dBFS")
    ap.add_argument("--pre", type=float, default=0.5, help="seconds of mic recorded before the tone starts (default 0.5)")
    ap.add_argument("--post", type=float, default=0.5, help="seconds of mic recorded after the tone ends (default 0.5)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    c = Rtsp(a.url, verbose=a.verbose)
    c.request("OPTIONS", a.url)
    hdr = {"Accept": "application/sdp"}
    if not a.no_require:
        hdr["Require"] = ONVIF_REQUIRE
    code, h, body = c.request("DESCRIBE", a.url, hdr)
    if code != 200:
        print("DESCRIBE -> %d" % code)
        return 1
    sdp = body.decode(errors="replace")
    medias = parse_sdp(sdp)
    if a.describe:
        print(sdp)
        for m in medias:
            print("  %-5s pt %s %s %s control=%s" % (m["kind"], m["pts"], m["rtpmap"], m["dir"] or "(no direction)", m["control"]))
        return 0

    mic = next((m for m in medias if m["kind"] == "audio" and m["dir"] != "sendonly"), None)
    back = next((m for m in medias if m["kind"] == "audio" and m["dir"] == "sendonly"), None)
    want_send = a.send is not None or a.tone is not None
    if a.record and not mic:
        print("no microphone audio track in the SDP")
        return 1
    if want_send and not back:
        print("no backchannel (a=sendonly audio) track in the SDP%s" % ("" if not a.no_require else " (expected without Require)"))
        return 1

    # SETUP: channels 0-1 for the mic track, 2-3 for the backchannel (go2rtc numbers them by media index)
    mic_ch = back_ch = -1
    if a.record and mic:
        code, h, _ = c.request("SETUP", c.control_url(mic["control"]), {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
        if code != 200:
            print("SETUP mic -> %d" % code)
            return 1
        mic_ch = 0
        print("mic track set up: %s" % h.get("transport", ""))
    if want_send and back:
        code, h, _ = c.request("SETUP", c.control_url(back["control"]), {"Transport": "RTP/AVP/TCP;unicast;interleaved=2-3"})
        if code != 200:
            print("SETUP backchannel -> %d" % code)
            return 1
        back_ch = 2
        print("backchannel set up: %s" % h.get("transport", ""))
    if mic_ch < 0 and back_ch < 0:
        print("nothing to do (use --record, --send or --tone)")
        return 1

    mic_pt = mic["pts"][0] if mic else 8
    back_pt = back["pts"][0] if back else 8
    rec = bytearray()
    stats = {"mic_pkts": 0, "mic_bytes": 0, "mic_lost": 0, "last_seq": None, "first_ms": 0.0}

    def on_frame(ch: int, data: bytes) -> None:
        if ch != mic_ch or len(data) < 12:
            return
        seq = struct.unpack(">H", data[2:4])[0]
        if stats["last_seq"] is not None and (seq - stats["last_seq"]) & 0xFFFF != 1:
            stats["mic_lost"] += ((seq - stats["last_seq"]) & 0xFFFF) - 1
        stats["last_seq"] = seq
        if not stats["mic_pkts"]:
            stats["first_ms"] = time.monotonic()
        stats["mic_pkts"] += 1
        cc = data[0] & 0x0F
        off = 12 + 4 * cc
        if data[0] & 0x10 and len(data) >= off + 4:   # extension
            off += 4 + 4 * struct.unpack(">H", data[off + 2:off + 4])[0]
        payload = data[off:]
        stats["mic_bytes"] += len(payload)
        rec.extend(g711_decode(payload, mic_pt))

    c.on_frame = on_frame
    code, h, _ = c.request("PLAY", a.url, {"Range": "npt=0.000-"})
    if code != 200:
        print("PLAY -> %d" % code)
        return 1
    t_play = time.monotonic()

    pcm = b""
    if a.send:
        pcm = wav_to_pcm8k(a.send)
    elif a.tone is not None:
        pcm = tone_pcm8k(a.tone, a.seconds, a.amp)
    frames = [pcm[i:i + 320] for i in range(0, len(pcm) - 319, 320)]   # 20 ms = 160 samples
    total = (a.pre if frames else 0) + len(frames) * 0.02 + (a.post if frames else 0)
    if not frames:
        total = a.seconds
    seq, ts, ssrc = 1, 0, 0x5EEDBEEF
    t_start = time.monotonic()
    sent = 0
    tone_t0 = tone_t1 = None
    deadline = t_start + total
    next_frame_at = t_start + (a.pre if frames else 0)
    while time.monotonic() < deadline:
        now = time.monotonic()
        if sent < len(frames) and now >= next_frame_at:
            if sent == 0:
                tone_t0 = now
            payload = g711_encode(frames[sent], back_pt)
            rtp = struct.pack(">BBHII", 0x80, back_pt | (0x80 if sent == 0 else 0), seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc) + payload
            c.sock.sendall(b"$" + bytes([back_ch]) + struct.pack(">H", len(rtp)) + rtp)
            seq += 1
            ts += 160
            sent += 1
            next_frame_at += 0.02
            if sent == len(frames):
                tone_t1 = time.monotonic()
            continue
        wait = max(0.0, min(next_frame_at - now if sent < len(frames) else deadline - now, 0.02))
        c.pump(wait if wait > 0 else 0.001)
    try:
        c.request("TEARDOWN", a.url)
    except (ConnectionError, OSError):
        pass
    c.sock.close()

    el = time.monotonic() - t_start
    print("sent %d backchannel frames (%.2f s of %s), received %d mic packets / %d bytes (%.2f s), lost %d, first mic packet %.0f ms after PLAY"
          % (sent, sent * 0.02, "PCMA" if back_pt == 8 else "PCMU", stats["mic_pkts"], stats["mic_bytes"], stats["mic_bytes"] / 8000.0,
             stats["mic_lost"], (stats["first_ms"] - t_play) * 1000 if stats["mic_pkts"] else -1))
    if a.record and rec:
        w = wave.open(a.record, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(bytes(rec))
        w.close()
        print("wrote %s (%.2f s, elapsed %.2f s)" % (a.record, len(rec) / 16000.0, el))
        if a.analyze:
            x = struct.unpack("<%dh" % (len(rec) // 2), bytes(rec))
            # the tone window in the recording, by wall clock relative to the first mic sample
            if tone_t0 is not None and stats["mic_pkts"]:
                s0 = int(max(0.0, tone_t0 - stats["first_ms"] + 0.3) * 8000)   # skip 300 ms of onset
                s1 = int(max(0.0, (tone_t1 or deadline) - stats["first_ms"] - 0.1) * 8000)
            else:
                s0, s1 = 0, len(x)
            seg = x[s0:s1] if s1 > s0 + 800 else x
            pre = x[:min(len(x), int(a.pre * 8000))] if frames else ()

            def dbfs(v: float) -> float:
                return 20 * math.log10(max(v, 1e-3) / 32768.0)

            def goertzel(sig, f):
                n = len(sig)
                k = int(0.5 + n * f / 8000.0)
                wv = 2 * math.pi * k / n
                cf = 2 * math.cos(wv)
                s1_ = s2 = 0.0
                for v in sig:
                    s0_ = v + cf * s1_ - s2
                    s2, s1_ = s1_, s0_
                return math.sqrt(max(s1_ * s1_ + s2 * s2 - cf * s1_ * s2, 0)) * 2 / n

            def rms(sig):
                return math.sqrt(sum(v * v for v in sig) / len(sig)) if sig else 0.0

            print("analysis window %.2f..%.2f s: @%.0f Hz %.1f dBFS, rms %.1f dBFS%s" % (
                s0 / 8000.0, (s0 + len(seg)) / 8000.0, a.analyze, dbfs(goertzel(seg, a.analyze)), dbfs(rms(seg)),
                (" | before the tone: @%.0f Hz %.1f dBFS, rms %.1f dBFS" % (a.analyze, dbfs(goertzel(pre, a.analyze)), dbfs(rms(pre)))) if len(pre) > 800 else ""))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
