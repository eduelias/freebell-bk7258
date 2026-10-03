"""Wait for the Beken ROM, identify, then dump flash 4K at a time (read-only).
Resumable: keeps a sparse image + a done-map; rerun to continue.

  bkdump.py [SIZE] [--port DEV]    SIZE overrides the flash size read from the JEDEC id (e.g. 0x800000)
"""
import argparse, sys, os, time, struct, json, zlib
from bkread import BK
IMG, MAP = "doorbell_flash.bin", "doorbell_flash.done.json"
ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("size", nargs="?", type=lambda s: int(s, 0), help="flash size override")
ap.add_argument("--port", help="UART adapter (default: SERIAL_PORT or the first USB serial device)")
args = ap.parse_args()
size_override = args.size
done = set(json.load(open(MAP))) if os.path.exists(MAP) else set()
bk = BK(port=args.port)
print(time.strftime("%T"), "waiting for ROM (replug battery now)...", flush=True)
t0 = time.time()
while not bk.link(seconds=1.0):
    if time.time() - t0 > 180: print("no ROM in 180s"); sys.exit(1)
tl = time.time(); print(time.strftime("%T"), "LINKED", flush=True)
for name, a in (("SYS_VERSION", 0x44010004), ("SCTRL_CHIP_ID", 0x800000)):
    v = bk.read_reg(a); print(" ", name, hex(v) if isinstance(v, int) else v, flush=True)
mid = bk.flash_mid(); print("  flash MID raw:", mid.hex(), flush=True)
size = size_override
if size is None and len(mid) == 15:
    cap = mid[13]  # JEDEC capacity byte (mid[11..13] = manuf, type, capacity)
    if 0x14 <= cap <= 0x19: size = 1 << cap
print("  flash size:", hex(size) if size else "unknown, defaulting to 4 MB", flush=True)
size = size or 0x400000
if not os.path.exists(IMG):
    with open(IMG, "wb") as f: f.truncate(size)
fails = 0; n = 0
with open(IMG, "r+b") as f:
    for addr in range(0, size, 0x1000):
        if addr in done: continue
        d = bk.read4k(addr)
        if d is None:
            fails += 1
            if fails >= 3:
                print(time.strftime("%T"), f"lost chip at {hex(addr)} after {time.time()-tl:.0f}s linked", flush=True); break
            bk.link(seconds=0.3); continue
        fails = 0; f.seek(addr); f.write(d); done.add(addr); n += 1
        if n % 32 == 0:
            json.dump(sorted(done), open(MAP, "w"))
            print(time.strftime("%T"), f"{hex(addr)}  {len(done)*4} KB / {size//1024} KB  ({time.time()-tl:.0f}s)", flush=True)
json.dump(sorted(done), open(MAP, "w"))
print(time.strftime("%T"), f"done this run: {n} sectors; total {len(done)*4} KB of {size//1024} KB", flush=True)
