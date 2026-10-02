"""Unattended GPIO mapping session using the firmware's own `pin` / `adc` commands (v0.2.0+).

  pinmap_session.py            run everything: scan, watch 40 s, pull classification, ADC sweep
  pinmap_session.py scan       one `pin all` snapshot
  pinmap_session.py watch 60   device-side edge watch for N seconds (press button / wave at PIR)
  pinmap_session.py pull       classify every safe pin: ext HIGH / ext LOW / floating
  pinmap_session.py adc        read every ADC channel that maps to a safe GPIO

Safe pins = everything except the console UART, ATE strap, QSPI0 and camera DVP pins
(the EXCLUDE set in pinscan.py).
"""
import glob, re, sys, time
import serial
from pinscan import EXCLUDE, NUM_GPIO

RE_PINS = re.compile(r"PINS ([01]{56})")
RE_IN = re.compile(r"PIN (\d+) in \w+ = (\d)")
RE_ADC = re.compile(r"ADC(\d+) raw=(\d+) mv=(\d+)")
ADC_CHAN_TO_GPIO = {13: "GPIO1", 8: "GPIO6", 9: "GPIO7", 10: "GPIO8/44", 11: "GPIO9/45", 6: "GPIO21", 4: "GPIO28"}
SAFE = [p for p in range(NUM_GPIO) if p not in EXCLUDE]


class Shell:
    def __init__(self):
        self.s = serial.Serial(glob.glob("/dev/cu.usbserial-*")[0], 115200, timeout=0.05)
        self.read(0.3)

    def read(self, sec):
        end = time.time() + sec
        buf = b""
        while time.time() < end:
            buf += self.s.read(4096)
        return buf.decode("latin-1").replace("\r", "")

    def cmd(self, line, wait=0.4):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n")
        return self.read(wait)

    def pins(self):
        for _ in range(3):
            m = RE_PINS.search(self.cmd("pin all"))
            if m:
                return m.group(1)
        return None


def show(bits):
    print("  high:", [p for p in range(NUM_GPIO) if bits[p] == "1"])
    print("  low :", [p for p in range(NUM_GPIO) if bits[p] == "0"])


def prep(sh):
    """Pins report 1 until they are input-enabled, so configure every safe pin as input + pull-up
    before scanning or watching (the pull test does this too, but scan/watch may run alone)."""
    for p in SAFE:
        sh.cmd(f"pin in {p} up", 0.08)


def scan(sh):
    print("== SCAN (pin all) ==")
    bits = sh.pins()
    if not bits:
        sys.exit("`pin all` did not answer: is firmware v0.2.0+ running?")
    print("  PINS", bits)
    show(bits)
    return bits


def watch(sh, secs):
    print(f"== WATCH {secs}s (device-side, 5 ms polling) ==")
    sh.s.reset_input_buffer()
    sh.s.write(f"pin watch {secs}\r\n".encode())
    out = sh.read(secs + 2)
    edges = re.findall(r"EDGE (\d+) ms pin (\d+) (\d)->(\d)", out)
    per_pin = {}
    for ms, p, a, b in edges:
        per_pin.setdefault(int(p), []).append(f"{int(ms)/1000:.2f}s {a}->{b}")
    if per_pin:
        for p, ev in sorted(per_pin.items()):
            print(f"  GPIO {p}: {len(ev)} edges, first {ev[:4]}")
    else:
        print("  no edges")
    return per_pin


def pull(sh):
    print("== PULL CLASSIFICATION (down, then up; alive check per pin) ==")
    cls = {}
    for p in SAFE:
        dn = RE_IN.search(sh.cmd(f"pin in {p} down"))
        up = RE_IN.search(sh.cmd(f"pin in {p} up"))
        if sh.pins() is None and sh.pins() is None:   # pins() already retries 3x; be very sure
            print(f"  !! shell stopped answering after GPIO {p}; aborting pull test")
            break
        if not (dn and up):
            cls[p] = "no reply"
            continue
        key = (int(dn.group(2)), int(up.group(2)))
        cls[p] = {(0, 1): "floating", (0, 0): "ext LOW", (1, 1): "ext HIGH"}.get(key, f"odd dn={key[0]} up={key[1]}")
    for k in ("ext HIGH", "ext LOW", "floating"):
        print(f"  {k:9s}:", [p for p, v in cls.items() if v == k])
    other = {p: v for p, v in cls.items() if v not in ("ext HIGH", "ext LOW", "floating")}
    if other:
        print("  other    :", other)
    return cls


def mux(sh):
    """Decode `pin mux` (firmware v0.3.0+): per-pin config byte from the AON GPIO registers."""
    print("== MUX (per-pin config: in/out enable, pull, 2nd-function) ==")
    rows = []
    for _ in range(3):
        out = sh.cmd("pin mux", 1.2)
        rows = re.findall(r"MUX (\d+): ((?:[0-9a-f]{2} ?){8})", out)
        if len(rows) == 7:
            break
    if not rows:
        sys.exit("`pin mux` did not answer: is firmware v0.3.0+ running?")
    cfg = {}
    for first, bytes_ in rows:
        for k, b in enumerate(bytes_.split()):
            cfg[int(first) + k] = int(b, 16)
    def desc(v):
        # NOTE: `pin out N 1` gives 0x03 and really drives the pin, `pin hiz` gives 0x09, so bits
        # 2/3 behave as active-low "stage disabled" flags on this chip. Labels below follow the
        # SDK's struct comments (active-high) and are therefore probably inverted for in/out:
        # treat them as "raw bit set", and trust measured behaviour.
        bits = []
        if v & 0x04: bits.append("bit2")
        if v & 0x08: bits.append("bit3")
        if v & 0x20: bits.append("pull-" + ("up" if v & 0x10 else "down"))
        if v & 0x40: bits.append("2ND-FUNC")
        return ",".join(bits) or "hi-Z"
    groups = {}
    for p, v in sorted(cfg.items()):
        groups.setdefault(desc(v), []).append(p)
    for d, pins in groups.items():
        print(f"  {d:22s}: {pins}")
    return cfg


def adc(sh):
    print("== ADC sweep ==")
    res = {}
    for ch, gpio in ADC_CHAN_TO_GPIO.items():
        out = sh.cmd(f"adc {ch}", 1.5)
        m = RE_ADC.search(out)
        if m:
            res[ch] = (int(m.group(2)), int(m.group(3)))
            print(f"  ADC{ch:<3} {gpio:9s} raw={m.group(2):>5} mv={m.group(3)}")
        else:
            err = " | ".join(l for l in out.splitlines() if l.strip() and "alive" not in l)
            print(f"  ADC{ch:<3} {gpio:9s} {err[:90]}")
    return res


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    sh = Shell()
    if mode == "scan":
        prep(sh); scan(sh)
    elif mode == "watch":
        prep(sh); watch(sh, int(sys.argv[2]) if len(sys.argv) > 2 else 40)
    elif mode == "pull":
        pull(sh)
    elif mode == "mux":
        mux(sh)
    elif mode == "adc":
        adc(sh)
    else:
        mux(sh)           # as booted: which pins are peripheral-muxed
        pull(sh)          # reclaims every safe pin as GPIO input + pull-up
        mux(sh)           # after reclaiming
        scan(sh)
        watch(sh, 40)
        adc(sh)
        print("== final check ==")
        print("  pin all answers:", sh.pins() is not None)


if __name__ == "__main__":
    main()
