"""Read-only Beken BootROM client (BK7258): link check, read register, flash MID, flash read 4K.
Implements ONLY read commands; no erase/write/register-write codes exist in this file."""
import serial, glob, time, struct, sys, zlib

class BK:
    def __init__(self, baud=115200):
        self.baud = baud; self.s = None; self.reopen()
    def reopen(self):
        while True:
            try:
                if self.s:
                    try: self.s.close()
                    except Exception: pass
                self.s = serial.Serial(glob.glob("/dev/cu.usbserial-*")[0], self.baud, timeout=0.05)
                return
            except Exception:
                time.sleep(0.1)
    def xfer(self, tx, rxlen, timeout=1.0):
        self.s.reset_input_buffer(); self.s.write(tx)
        buf = b""; end = time.time() + timeout
        while len(buf) < rxlen and time.time() < end:
            buf += self.s.read(rxlen - len(buf))
        return buf
    def link(self, seconds=0.5):
        """Spam LinkCheck every 3 ms without flushing (the ROM listens only briefly after power-up)."""
        reply = bytes.fromhex("040e0501e0fc0100"); buf = b""; end = time.time() + seconds
        self.s.reset_input_buffer()
        while time.time() < end:
            try:
                self.s.write(bytes.fromhex("01e0fc0100")); time.sleep(0.003)
                buf += self.s.read(self.s.in_waiting or 0)
            except Exception:
                print(time.strftime("%T"), "adapter dropped, reopening", flush=True); self.reopen(); buf = b""
            if reply in buf:
                time.sleep(0.05); self.s.reset_input_buffer()  # drop replies to the extra probes
                return True
        return False
    def read_reg(self, addr):
        tx = bytes.fromhex("01e0fc0503") + struct.pack("<I", addr)
        r = self.xfer(tx, 15)
        if len(r) == 15 and r[:7] == bytes.fromhex("040e0c01e0fc03"):
            return struct.unpack("<I", r[11:15])[0]
        return ("raw", r.hex())
    def flash_mid(self):
        tx = bytes.fromhex("01e0fcfff40500") + bytes([0x0e]) + struct.pack("<I", 0x9f)
        r = self.xfer(tx, 15)
        return r
    def read4k(self, addr):
        tx = bytes.fromhex("01e0fcfff40500") + bytes([0x09]) + struct.pack("<I", addr)
        r = self.xfer(tx, 4111, 5.0)
        if len(r) == 4111 and r[:7] == bytes.fromhex("040effe001e0fc")[:0] + r[:7] and r[9] == 0x09 and r[10] == 0x00 \
           and struct.unpack("<I", r[11:15])[0] == addr:
            return r[15:]
        return None

if __name__ == "__main__":
    bk = BK()
    print("link:", bk.link())
    for name, a in (("SYS_VERSION", 0x44010004), ("SCTRL_CHIP_ID", 0x800000)):
        v = bk.read_reg(a); print(name, hex(v) if isinstance(v, int) else v)
    m = bk.flash_mid(); print("flash MID raw:", m.hex())
    d = bk.read4k(0)
    if d:
        print("sector0 ok, crc32", hex(zlib.crc32(d)), "first 64:", d[:64].hex())
        print("ascii:", bytes(c if 32 <= c < 127 else 46 for c in d[:256]).decode())
    else:
        print("sector0 read failed")
