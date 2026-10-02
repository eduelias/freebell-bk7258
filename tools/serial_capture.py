import serial, sys, time, glob
baud, secs, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
end = time.time() + secs; buf = b""; s = None
with open(out, "ab") as f:
    while time.time() < end:
        if s is None:
            ports = glob.glob("/dev/cu.usbserial-*")
            if not ports: time.sleep(0.3); continue
            try: s = serial.Serial(ports[0], baud, timeout=0.2); print(f"{time.strftime('%T')} opened {ports[0]}", flush=True)
            except Exception as e: time.sleep(0.3); continue
        try: d = s.read(4096)
        except Exception as e:
            print(f"{time.strftime('%T')} lost port: {e}", flush=True); s = None; continue
        if d: f.write(d); f.flush(); buf += d
txt = buf.decode("latin-1")
print(f"{baud}: {len(buf)} bytes, printable {sum(c.isprintable() or c in chr(13)+chr(10)+chr(9) for c in txt)/max(len(txt),1):.2f}")
print(txt[-4000:])
