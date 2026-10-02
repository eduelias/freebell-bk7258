# Beken ROM download-mode probe: spam LinkCheck (01 E0 FC 01 00) at 115200,
# watch for the ROM's reply (04 0E 05 01 E0 FC 01 00). Read-only, writes nothing to flash.
import serial, time, glob
LINK = bytes.fromhex("01e0fc0100")
REPLY = bytes.fromhex("040e0501e0fc01")
secs = 90
s = None
while s is None:
    try:
        p = glob.glob("/dev/cu.usbserial-*")[0]
        s = serial.Serial(p, 115200, timeout=0)
    except Exception as e:
        print(time.strftime("%T"), "open failed, retrying:", e, flush=True); time.sleep(0.5)
print(time.strftime("%T"), "probing on", p, flush=True)
buf = b""; end = time.time() + secs; sent = 0; found = None
with open("linkcheck_rx.bin", "wb") as f:
    while time.time() < end:
        s.write(LINK); sent += 1
        time.sleep(0.003)
        d = s.read(4096)
        if d:
            f.write(d); f.flush(); buf += d
            if REPLY in buf and not found:
                found = time.strftime("%T"); print(found, "ROM REPLIED:", buf[buf.index(REPLY):][:12].hex(), flush=True)
                # keep the chip in download mode: stop spamming after a few more checks
                for _ in range(5): s.write(LINK); time.sleep(0.01)
                break
print(time.strftime("%T"), f"sent {sent} probes, received {len(buf)} bytes", flush=True)
print("first bytes:", buf[:64].hex(), flush=True)
print("found" if found else "no ROM reply", flush=True)
