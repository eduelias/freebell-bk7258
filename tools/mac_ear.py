#!/usr/bin/env python3
"""Analyse a Mac-microphone recording of the doorbell speaker (the "ear" of the audio test rig).

The recording is made in Terminal.app (microphone permission):
  ffmpeg -f avfoundation -i ":1" -t 30 -ac 1 -ar 16000 rec.wav
Each test is preceded by a 500 Hz Mac beep (audio/marker_500hz_0p3s.wav) so the timeline is
self-contained. For every marker this prints the loudness and the strongest spectral bins of the
window that follows, plus the energy at an expected frequency.

  python3 tools/mac_ear.py rec.wav [--labels "A,B,C"] [--expect 1000] [--window 1.0:2.8]
"""
import argparse, math, struct, wave


def goertzel(seg, f, sr):
    n = len(seg)
    k = int(0.5 + n * f / sr)
    w = 2 * math.pi * k / n
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in seg:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - c * s1 * s2, 0)) / n


def rms(seg):
    return math.sqrt(sum(v * v for v in seg) / len(seg)) if seg else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--labels", default="")
    ap.add_argument("--expect", type=float, default=1000.0)
    ap.add_argument("--window", default="1.0:2.8", help="seconds after the marker: start:end")
    ap.add_argument("--marker", type=float, default=500.0)
    a = ap.parse_args()
    w = wave.open(a.wav)
    sr, n = w.getframerate(), w.getnframes()
    x = struct.unpack("<%dh" % n, w.readframes(n))
    labels = [s.strip() for s in a.labels.split(",")] if a.labels else []
    w0, w1 = (float(v) for v in a.window.split(":"))

    step = sr // 4
    marks, last = [], -9.0
    for i in range(0, n - step, step):
        t = i / sr
        if goertzel(x[i:i + step], a.marker, sr) > 30 and t - last > 2.0:
            marks.append(t)
            last = t
    print(f"{n / sr:.1f}s at {sr} Hz, {len(marks)} markers at {[round(m, 1) for m in marks]}")
    for i, t in enumerate(marks):
        seg = x[int((t + w0) * sr):int((t + w1) * sr)]
        if not seg:
            continue
        sp = [(f, goertzel(seg, f, sr)) for f in range(100, min(7900, sr // 2 - 100), 100)]
        top = sorted(sp, key=lambda p: -p[1])[:5]
        lab = labels[i] if i < len(labels) else f"marker {i}"
        print(f"{lab:28s} rms {rms(seg):6.0f}  @{a.expect:.0f}Hz {goertzel(seg, a.expect, sr):6.1f}  top: "
              + ", ".join(f"{f}Hz:{m:.0f}" for f, m in top))


if __name__ == "__main__":
    main()
