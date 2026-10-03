"""LED hunt with a webcam as the eyes.

A capture loop (run in Terminal.app, which macOS lets use the camera) keeps rewriting
CAM_DIR/latest.jpg every ~1.5 s, e.g. with CAM_DIR=/tmp/doorbell-cam:

  while true; do imagesnap -q -w 0.5 $CAM_DIR/new.jpg && \
    mv -f $CAM_DIR/new.jpg $CAM_DIR/latest.jpg; sleep 0.5; done

Metric: percentage of bright pixels in the lower part of the frame (where the board sits), so a
person walking through the background or a small camera shift does not count as a change. For
each candidate pin: drive it (low and/or high), grab a fresh frame, park it hi-Z again, compare
with the baseline taken at the start. The white fill LED saturates the sensor, so a pin that
turns it OFF shows as a large drop; the dim blue ring LEDs need `--region` tuned to them.

  led_by_camera.py --cam-dir DIR [--low] [--high] [--pins 5 8 9 ...] [--keep 9] [--region 0.6]
                   [--box x0 y0 x1 y1] [--port DEV]
     --cam-dir DIR the directory the capture loop writes latest.jpg to; frames that show a change
                   are saved under DIR/hunt/ (required)
     --low/--high  which levels to drive (default both)
     --keep N      drive GPIO N high first and skip it (power-enable lines; default 9)
     --region F    use the bottom (1-F) fraction of the frame (default 0.6 = lower 40 %)
     --box ...     instead of --region: a frame-fraction box, e.g. the white LED flare 0.48 0.80 0.75 1.0
     --port DEV    UART adapter (default: SERIAL_PORT or the first USB serial device)
"""
import argparse, os, sys, time
import serial
from PIL import Image
import dbenv

LATEST = OUT = None   # set from --cam-dir in main()
CONSOLE = {10, 11}
NO_SINK = {0, 3, 4, 12, 15, 24, 27, 44, 45, 53} | set(range(29, 40))


class Shell:
    def __init__(self, port=None):
        self.s = serial.Serial(dbenv.serial_port(port), 115200, timeout=0.05)
        self.cmd("", 0.3)

    def cmd(self, line, wait=0.3):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n")
        end = time.time() + wait
        while time.time() < end:
            self.s.read(4096)


def fresh_frame(after_ts, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if os.path.getmtime(LATEST) > after_ts + 0.2:
                time.sleep(0.15)
                return Image.open(LATEST).convert("L")
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    raise RuntimeError("no fresh camera frame; is the capture loop running in Terminal.app?")


def bright(im, region):
    """region: a bottom fraction (float) or a (x0, y0, x1, y1) box in frame fractions."""
    w, h = im.size
    if isinstance(region, tuple):
        box = (int(w * region[0]), int(h * region[1]), int(w * region[2]), int(h * region[3]))
    else:
        box = (0, int(h * region), w, h)
    crop = im.crop(box).resize((240, 120))
    hist = crop.histogram()
    n = 240 * 120
    mean = sum(k * v for k, v in enumerate(hist)) / n
    return 100.0 * sum(hist[240:]) / n, mean


def main():
    global LATEST, OUT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cam-dir", required=True, help="directory the capture loop writes latest.jpg to")
    ap.add_argument("--low", action="store_true")
    ap.add_argument("--high", action="store_true")
    ap.add_argument("--pins", type=int, nargs="+")
    ap.add_argument("--keep", type=int, action="append")
    ap.add_argument("--region", type=float, default=0.6)
    ap.add_argument("--box", type=float, nargs=4, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--port")
    a = ap.parse_args()
    cam = os.path.expanduser(a.cam_dir)
    LATEST, OUT = os.path.join(cam, "latest.jpg"), os.path.join(cam, "hunt")
    levels = [lv for lv, f in ((0, a.low), (1, a.high)) if f] or [0, 1]
    keep = set(a.keep) if a.keep else {9}
    region = tuple(a.box) if a.box else a.region
    pins = a.pins or [p for p in range(56) if p not in CONSOLE | keep | NO_SINK]
    os.makedirs(OUT, exist_ok=True)
    sh = Shell(a.port)
    for k in keep:
        sh.cmd(f"pin out {k} 1")
    for p in pins:
        sh.cmd(f"pin hiz {p}", 0.12)
    time.sleep(2.0)
    base = bright(fresh_frame(time.time()), region)
    print(f"baseline: {base[0]:.2f}% saturated, mean {base[1]:.1f}   (region {region})")
    print(f"testing {len(pins)} pins, levels {levels}")

    results = []
    for p in pins:
        for lv in levels:
            t0 = time.time()
            sh.cmd(f"pin out {p} {lv}")
            try:
                time.sleep(0.6)
                im = fresh_frame(t0 + 0.6)
            except RuntimeError as e:
                print("  ", e)
                sh.cmd(f"pin hiz {p}")
                return
            sh.cmd(f"pin hiz {p}")
            b = bright(im, region)
            d0, d1 = b[0] - base[0], b[1] - base[1]
            flag = "  <== change" if abs(d0) > 8 or abs(d1) > 12 else ""
            print(f"  GPIO {p:2d} = {lv}: sat {b[0]:5.2f}% ({d0:+5.2f})  mean {b[1]:6.1f} ({d1:+6.1f}){flag}", flush=True)
            results.append((abs(d0) + abs(d1), p, lv, d0, d1))
            if flag:
                im.save(os.path.join(OUT, f"gpio{p:02d}_{lv}.jpg"))
            time.sleep(0.8)
    results.sort(reverse=True)
    print("\nlargest changes:")
    for _, p, lv, d0, d1 in results[:8]:
        print(f"  GPIO {p:2d} = {lv}: sat {d0:+5.2f}%  mean {d1:+6.1f}")


if __name__ == "__main__":
    main()
