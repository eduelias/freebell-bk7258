"""Interactive LED hunt: blinks one candidate pin at a time while you watch the board.

Run it in a terminal next to the doorbell (firmware v0.5.0+ on UART0):

    python3 tools/led_hunt.py            # all floating candidates
    python3 tools/led_hunt.py 5 8 2 6    # only these pins

For each pin it blinks 6 times (0.3 s on / 0.3 s off), then asks what you saw:
  w = white fill light, b = blue ring LED(s), o = other (IR, click, buzz...), n = nothing,
  r = repeat this pin, q = quit.  Answers are saved to led_hunt_results.txt in the current dir.
GPIO 9 (sensor-rail enable) is held high and skipped; the console pins and pins that are driven
by something else on the board are skipped too.
"""
import glob, re, sys, time
import serial

CONSOLE = {10, 11}
KEEP_HIGH = {9}
NO_SINK = {0, 3, 4, 12, 15, 24, 27, 44, 45, 53} | set(range(29, 40))


def main():
    s = serial.Serial(glob.glob("/dev/cu.usbserial-*")[0], 115200, timeout=0.05)

    def cmd(line, wait=0.3):
        s.reset_input_buffer()
        s.write(line.encode() + b"\r\n")
        end = time.time() + wait
        buf = b""
        while time.time() < end:
            buf += s.read(4096)
        return buf.decode("latin-1")

    cmd("\r\n")
    for k in KEEP_HIGH:
        cmd(f"pin out {k} 1")
    if len(sys.argv) > 1:
        pins = [int(a) for a in sys.argv[1:]]
    else:
        pins = [p for p in range(56) if p not in CONSOLE | KEEP_HIGH | NO_SINK]
    print(f"{len(pins)} pins to try: {pins}\nWatch the board. Press Enter to start.")
    input()
    results = {}
    i = 0
    while i < len(pins):
        p = pins[i]
        print(f"\n--- GPIO {p}: blinking 6x now ...", flush=True)
        cmd(f"pin blink {p} 6", 4.2)
        cmd(f"pin hiz {p}")
        ans = input("    saw: [w]hite [b]lue [o]ther [n]othing [r]epeat [q]uit > ").strip().lower()[:1]
        if ans == "q":
            break
        if ans == "r":
            continue
        results[p] = {"w": "white", "b": "blue", "o": "other"}.get(ans, "nothing")
        with open("led_hunt_results.txt", "a") as f:
            f.write(f"{time.strftime('%F %T')} GPIO {p}: {results[p]}\n")
        i += 1
    hits = {p: r for p, r in results.items() if r != "nothing"}
    print("\nhits:", hits or "none", "\n(saved to led_hunt_results.txt)")


if __name__ == "__main__":
    main()
