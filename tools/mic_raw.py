#!/usr/bin/env python3
"""Inspect a raw `mic rec` capture (32-bit ADC FIFO words: lo16 = L/MIC1, hi16 = R/MIC2) and
optionally export one half as a WAV.

  python3 tools/mic_raw.py capture.raw --seconds 4 [--expect 1000] [--wav out.wav] [--half lo|hi]

--seconds is the capture length (the `mic rec MS`): the true sample rate is derived from it
(the UART log also prints "words/s"). Prints per-0.5 s rms, the strongest bin and the energy at
the expected frequency with purity = goertzel/rms (0.71 is a pure sine).
"""
import argparse, math, os, struct, wave


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--seconds", type=float, required=True)
    ap.add_argument("--expect", type=float, default=1000.0)
    ap.add_argument("--half", choices=("lo", "hi"), default="lo")
    ap.add_argument("--wav")
    a = ap.parse_args()
    d = open(os.path.expanduser(a.raw), "rb").read()
    n = len(d) // 4
    words = struct.unpack("<%dI" % n, d[:n * 4])
    sr = n / a.seconds
    lo = [struct.unpack("<h", struct.pack("<H", x & 0xFFFF))[0] for x in words]
    hi = [struct.unpack("<h", struct.pack("<H", x >> 16))[0] for x in words]
    v = lo if a.half == "lo" else hi
    print(f"{n} words in {a.seconds:.1f}s -> {sr:.0f} samples/s; lo16 rms {math.sqrt(sum(s*s for s in lo)/n):.0f}, "
          f"hi16 rms {math.sqrt(sum(s*s for s in hi)/n):.0f}")
    step = int(sr / 2)
    for i in range(0, len(v) - step, step):
        seg = v[i:i + step]
        r = math.sqrt(sum(s * s for s in seg) / len(seg))
        sp = [(f, goertzel(seg, f, sr)) for f in range(200, int(sr / 2) - 200, 50)]
        f, m = max(sp, key=lambda p: p[1])
        e = goertzel(seg, a.expect, sr)
        print(f"  {i / sr:4.1f}s rms {r:6.0f}  @{a.expect:.0f}Hz {e:7.0f} purity {e / r if r else 0:.2f}  strongest {f} Hz {m:6.0f}")
    if a.wav:
        mean = sum(v) / len(v)
        y = [s - mean for s in v]
        pk = max(abs(s) for s in y) or 1
        sc = min(1.0, 28000 / pk)
        with wave.open(os.path.expanduser(a.wav), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(int(round(sr)))
            w.writeframes(b"".join(struct.pack("<h", int(s * sc)) for s in y))
        print(f"wrote {a.wav} at {int(round(sr))} Hz (scale {sc:.2f})")


if __name__ == "__main__":
    main()
