"""Capture raw UART bytes for a while, tolerating adapter drop-outs (board resets).

  serial_capture.py BAUD SECONDS OUTFILE [--port DEV]   (port: SERIAL_PORT or the first USB serial device)
"""
import argparse, serial, sys, time
import dbenv
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("baud", type=int); ap.add_argument("seconds", type=float); ap.add_argument("out")
ap.add_argument("--port", help="UART adapter")
args = ap.parse_args()
baud, secs, out = args.baud, args.seconds, args.out
end = time.time() + secs; buf = b""; s = None
with open(out, "ab") as f:
    while time.time() < end:
        if s is None:
            port = dbenv.serial_port(args.port, required=False)
            if not port: time.sleep(0.3); continue
            try: s = serial.Serial(port, baud, timeout=0.2); print(f"{time.strftime('%T')} opened {port}", flush=True)
            except Exception as e: time.sleep(0.3); continue
        try: d = s.read(4096)
        except Exception as e:
            print(f"{time.strftime('%T')} lost port: {e}", flush=True); s = None; continue
        if d: f.write(d); f.flush(); buf += d
txt = buf.decode("latin-1")
print(f"{baud}: {len(buf)} bytes, printable {sum(c.isprintable() or c in chr(13)+chr(10)+chr(9) for c in txt)/max(len(txt),1):.2f}")
print(txt[-4000:])
