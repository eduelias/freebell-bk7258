"""Find LED-driving pins without eyes, using the on-board light sensor (GPIO 8 / ADC10) as a meter.

After boot the SDK pin map drives most pins low, which lights the active-low LEDs (white fill
light + four blue ring LEDs, per the user). Phase 1 parks every non-console pin high-impedance one
at a time and reads the light sensor after each: the pin whose release makes the reading fall is
the one that was sinking a visible LED. Phase 2 (from the all-hi-Z state) sinks each floating
candidate for a moment and looks for the reading to rise. Everything is left hi-Z at the end.

Usage: led_by_light.py [--phase2] [--keep N ...]   (firmware v0.5.0+)
  --adc N    use SARADC channel N as the meter (default 10 = light sensor; 9 = supply sense)
  --keep N   drive GPIO N high first and leave it out of the sweeps (power-enable lines, e.g. 9)
"""
import glob, re, statistics, sys, time
import serial

CONSOLE = {10, 11}
# never sink these as outputs: externally held low/high in pinmap_session4 (contention risk)
NO_SINK = {0, 3, 4, 12, 15, 24, 27, 44, 45, 53} | set(range(29, 40))
ADC_CH = next((int(sys.argv[i + 1]) for i, a in enumerate(sys.argv) if a == "--adc"), 10)
RE_ADC = re.compile(rf"ADC{ADC_CH} raw=(\d+) mv=(\d+)")


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

    def cmd(self, line, wait=0.3):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n")
        return self.read(wait)

    def light(self, n=3):
        vals = []
        for _ in range(n):
            m = RE_ADC.search(self.cmd(f"adc {ADC_CH}", 1.2))
            if m:
                vals.append(int(m.group(2)))
        return statistics.median(vals) if vals else None


def main():
    sh = Shell()
    keep = {int(sys.argv[i + 1]) for i, a in enumerate(sys.argv) if a == "--keep"}
    for k in sorted(keep):
        sh.cmd(f"pin out {k} 1", 0.3)
        print(f"holding GPIO {k} high (power enable)")
    pins = [p for p in range(56) if p not in CONSOLE and p not in keep]
    base = sh.light(5)
    print(f"baseline light (boot state, LEDs presumably on): {base} mV")
    if base is None:
        sys.exit("light sensor not answering")

    print("== phase 1: release pins to hi-Z one at a time ==")
    prev = base
    hits = []
    for p in pins:
        sh.cmd(f"pin hiz {p}", 0.2)
        v = sh.light(2)
        if v is None:
            continue
        delta = v - prev
        flag = ""
        if abs(delta) >= max(15, 0.15 * max(prev, 1)):
            flag = "  <== light changed"
            hits.append((p, prev, v))
        print(f"  hiz {p:2d}: {v:5.0f} mV  (delta {delta:+5.0f}){flag}")
        prev = v
    allhiz = sh.light(5)
    print(f"all pins hi-Z: {allhiz} mV   phase-1 hits: {hits}")

    if "--phase2" in sys.argv or hits == []:
        print("== phase 2: sink floating candidates one at a time (from all hi-Z) ==")
        cands = [p for p in pins if p not in NO_SINK]
        for p in cands:
            sh.cmd(f"pin out {p} 0", 0.2)
            v = sh.light(2)
            sh.cmd(f"pin hiz {p}", 0.2)
            if v is None:
                continue
            delta = v - allhiz
            flag = "  <== lights something" if delta >= max(15, 0.15 * max(allhiz, 1)) else ""
            print(f"  sink {p:2d}: {v:5.0f} mV  (delta {delta:+5.0f}){flag}")
    print("done; all pins left hi-Z")


if __name__ == "__main__":
    main()
