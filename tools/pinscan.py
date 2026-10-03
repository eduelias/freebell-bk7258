"""Map the doorbell's GPIOs through the Armino shell's `gpio` command.

Modes:
  pinscan.py scan                 read every allowed pin once (no reconfiguration)
  pinscan.py diff "press button"  snapshot, wait for Enter, snapshot again, show changed pins
  pinscan.py watch 20             poll all pins for N seconds, print each transition (PIR, button)
  pinscan.py blink 14 [count]     pulse one pin as output (LED hunting), then leave it low
  pinscan.py pull 14 up|down      reconfigure one pin as input with a pull and read it

Pins never touched (see README): 0 (ATE strap), 10/11 (UART0 console), 12/13 (UART0 flow
control), 22-27 (QSPI0), 27/29-39 (camera DVP). Use --allow N to override for one pin.
"""
import argparse, re, sys, time
import serial
import dbenv

NUM_GPIO = 56
EXCLUDE = {0, 10, 11, 12, 13, 22, 23, 24, 25, 26, 27} | set(range(29, 40))
RE_IN = re.compile(r"gpio input value is\s*([0-9a-fA-F]+)")


class Shell:
    def __init__(self, port=None, baud=115200):
        self.s = serial.Serial(dbenv.serial_port(port), baud, timeout=0.05)
        self.drain(0.3)

    def drain(self, sec):
        end = time.time() + sec
        buf = b""
        while time.time() < end:
            buf += self.s.read(4096)
        return buf

    def cmd(self, line, wait=0.25):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n")
        return self.drain(wait).decode("latin-1")

    def read_pin(self, pin):
        for _ in range(3):
            m = RE_IN.search(self.cmd(f"gpio input_get {pin}"))
            if m:
                return int(m.group(1), 16) & 1
        return None

    def snapshot(self, pins):
        return {p: self.read_pin(p) for p in pins}


def allowed(args):
    ex = EXCLUDE - set(args.allow or [])
    return [p for p in range(NUM_GPIO) if p not in ex]


def show(snap):
    cells = []
    for p in sorted(snap):
        v = snap[p]
        cells.append(f"{p:2d}:{'?' if v is None else v}")
    for i in range(0, len(cells), 14):
        print("  " + "  ".join(cells[i:i + 14]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["scan", "diff", "watch", "blink", "pull"])
    ap.add_argument("arg", nargs="*")
    ap.add_argument("--allow", type=int, action="append", help="also touch this normally excluded pin")
    ap.add_argument("--port", help="UART adapter (default: SERIAL_PORT or the first USB serial device)")
    a = ap.parse_args()
    sh = Shell(a.port)
    pins = allowed(a)

    if a.mode == "scan":
        snap = sh.snapshot(pins)
        print(f"{len(pins)} pins read ({sum(v == 1 for v in snap.values())} high, "
              f"{sum(v == 0 for v in snap.values())} low, {sum(v is None for v in snap.values())} unreadable):")
        show(snap)

    elif a.mode == "diff":
        label = " ".join(a.arg) or "do the thing"
        before = sh.snapshot(pins)
        print("baseline taken. Now:", label)
        input("  ...hold it / keep doing it, then press Enter here: ")
        after = sh.snapshot(pins)
        changed = {p: (before[p], after[p]) for p in pins if before[p] != after[p]}
        if changed:
            for p, (b, c) in changed.items():
                print(f"  GPIO {p}: {b} -> {c}")
        else:
            print("  no pin changed")

    elif a.mode == "watch":
        secs = float(a.arg[0]) if a.arg else 20
        last = sh.snapshot(pins)
        print(f"watching {len(pins)} pins for {secs:.0f}s (press the button, wave at the PIR)...")
        t0 = time.time()
        while time.time() - t0 < secs:
            cur = sh.snapshot(pins)
            for p in pins:
                if cur[p] != last[p]:
                    print(f"  {time.time() - t0:6.1f}s  GPIO {p}: {last[p]} -> {cur[p]}")
            last = cur
        print("done")

    elif a.mode == "blink":
        pin = int(a.arg[0]); count = int(a.arg[1]) if len(a.arg) > 1 else 6
        if pin not in pins:
            sys.exit(f"GPIO {pin} is excluded; pass --allow {pin} if you really mean it")
        print(f"blinking GPIO {pin} x{count} (watch the board)...")
        sh.cmd(f"gpio output {pin}", 0.1)   # output_high/low do not set the direction themselves
        for _ in range(count):
            sh.cmd(f"gpio output_high {pin}", 0.05); time.sleep(0.4)
            sh.cmd(f"gpio output_low {pin}", 0.05); time.sleep(0.4)
        print("left low")

    elif a.mode == "pull":
        pin = int(a.arg[0]); direction = a.arg[1] if len(a.arg) > 1 else "up"
        if pin not in pins:
            sys.exit(f"GPIO {pin} is excluded; pass --allow {pin} if you really mean it")
        print(sh.cmd(f"gpio input {pin} pull_{direction}", 0.3).strip())   # CLI wants pull_up / pull_down
        print(f"GPIO {pin} with pull-{direction}: {sh.read_pin(pin)}")


if __name__ == "__main__":
    main()
