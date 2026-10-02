"""Beken BootROM flash WRITER for the BK7258 doorbell (GD25WQ64E, 8 MB).

Ported from BK7231GUIFlashTool's BK7258 ("direct flash access") path:
  read MID -> save status-register protection bits -> clear only the protection bits
  -> per 4K sector: skip if identical, else erase, write, read back and compare
  -> restore the original protection bits.

Safety rails:
  * DRY RUN unless --write is given.
  * Only touches [--start, --start+len(image)) and refuses 0x7C0000-0x800000
    (stock per-device KV + RF calibration) unless --allow-tail.
  * Never touches eFuse/OTP. BK7258 download mode is in ROM, so a bad flash is
    always recoverable by restoring the backup image.

Usage:
  bkwrite.py IMAGE --start 0x0                 # dry run: report differing sectors
  bkwrite.py IMAGE --start 0x0 --write         # actually flash
  bkwrite.py BACKUP.bin --start 0x0 --write --allow-tail   # full restore
"""
import argparse, struct, sys, time
from bkread import BK

SECTOR = 0x1000
FLASH_SIZE = 0x800000
GD25WQ64E_MID = 0x001765C8
# BKFlashList: GD25WQ64E szSR=2, cwMsk = BIT(14) | BFD(0x1f, 2, 5), unprotect = 0x00
PROT_MASK = (1 << 14) | (0x1F << 2)
SR_READ_CMDS = (0x05, 0x35)
SR_WRITE_CMD = 0x01
TAIL_START = 0x7C0000

HDR = bytes.fromhex("040eff01e0fcf4")


class BKW(BK):
    def flash_mid_int(self):
        # Same as the GUI tool: ToInt32(buf, 11) >> 8. Length byte is 5, or 6 on
        # BootROM revisions that report one extra byte (ours does: 0x06).
        r = self.flash_mid()
        if len(r) == 15 and r[:7] == HDR and r[7] in (5, 6) and r[9] == 0x0E and r[10] == 0:
            return struct.unpack("<I", r[11:15])[0] >> 8
        return None

    def read_sr_byte(self, cmd):
        tx = bytes.fromhex("01e0fcfff40200") + bytes([0x0C, cmd])
        r = self.xfer(tx, 13)
        if len(r) == 13 and r[:7] == HDR and r[9] == 0x0C and r[10] == 0 and r[11] == cmd:
            return r[12]
        return None

    def read_status(self):
        st = 0
        for i, cmd in enumerate(SR_READ_CMDS):
            b = self.read_sr_byte(cmd)
            if b is None:
                return None
            st |= b << (8 * i)
        return st

    def write_status16(self, val):
        tx = bytes.fromhex("01e0fcfff40400") + bytes([0x0D, SR_WRITE_CMD, val & 0xFF, (val >> 8) & 0xFF])
        r = self.xfer(tx, 14)
        return (len(r) == 14 and r[:7] == HDR and r[9] == 0x0D and r[10] == 0
                and r[11] == SR_WRITE_CMD and r[12] == (val & 0xFF) and r[13] == ((val >> 8) & 0xFF))

    def set_protection(self, target_bits, tries=10):
        target = target_bits & PROT_MASK
        for _ in range(tries):
            st = self.read_status()
            if st is None:
                time.sleep(0.01); continue
            if (st & PROT_MASK) == target:
                return st
            self.write_status16((st & ~PROT_MASK) | target)
            time.sleep(0.02)
        return None

    def erase4k(self, addr):
        tx = bytes.fromhex("01e0fcfff40500") + bytes([0x0B]) + struct.pack("<I", addr)
        r = self.xfer(tx, 15, 2.0)
        return len(r) >= 10 and r[:7] == HDR and r[7] == 0x06 and r[9] == 0x0B and (len(r) < 11 or r[10] == 0)

    def write4k(self, addr, data):
        assert len(data) == SECTOR
        tx = bytes.fromhex("01e0fcfff4") + struct.pack("<H", 1 + 4 + SECTOR) + bytes([0x07]) + struct.pack("<I", addr) + data
        r = self.xfer(tx, 15, 3.0)
        return (len(r) == 15 and r[:7] == HDR and r[9] == 0x07 and r[10] == 0
                and struct.unpack("<I", r[11:15])[0] == addr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--start", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--write", action="store_true", help="actually erase/write (default: dry run)")
    ap.add_argument("--allow-tail", action="store_true", help="allow writing 0x7C0000-0x800000")
    ap.add_argument("--wait", type=float, default=180, help="seconds to wait for the ROM link")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    if len(img) % SECTOR:
        img += b"\xff" * (SECTOR - len(img) % SECTOR)
    end = a.start + len(img)
    if a.start % SECTOR or a.start < 0 or end > FLASH_SIZE:
        sys.exit(f"range {hex(a.start)}-{hex(end)} invalid for 8 MB flash")
    if end > TAIL_START and not a.allow_tail:
        sys.exit(f"range ends at {hex(end)}, overlapping stock KV/RF area at {hex(TAIL_START)}; use --allow-tail to override")
    print(f"image {a.image}: {len(img)//1024} KB -> flash {hex(a.start)}-{hex(end)}  mode={'WRITE' if a.write else 'DRY RUN'}", flush=True)

    bk = BKW()
    print(time.strftime("%T"), "waiting for ROM (press the doorbell button)...", flush=True)
    t0 = time.time()
    while not bk.link(seconds=1.0):
        if time.time() - t0 > a.wait:
            sys.exit("no ROM reply")
    print(time.strftime("%T"), "LINKED", flush=True)

    mid = bk.flash_mid_int()
    print("flash MID:", hex(mid) if mid is not None else None, flush=True)
    if mid != GD25WQ64E_MID:
        sys.exit("unexpected flash MID; refusing to continue (flash definition is GD25WQ64E only)")
    orig = bk.read_status()
    if orig is None:
        sys.exit("could not read flash status register")
    print(f"status register {hex(orig)}, protection bits {hex(orig & PROT_MASK)}", flush=True)

    # pass 1: find differing sectors
    todo = []
    for off in range(0, len(img), SECTOR):
        addr = a.start + off
        cur = bk.read4k(addr)
        if cur is None:
            sys.exit(f"read failed at {hex(addr)} while comparing")
        if cur != img[off:off + SECTOR]:
            todo.append(addr)
    print(f"{len(todo)} of {len(img)//SECTOR} sectors differ", flush=True)
    if not a.write or not todo:
        print("dry run: nothing written" if not a.write else "already identical", flush=True)
        return

    if (orig & PROT_MASK) != 0:
        print("clearing flash protection...", flush=True)
        if bk.set_protection(0) is None:
            sys.exit("could not clear flash protection; nothing written")
    ok = 0
    try:
        for i, addr in enumerate(todo, 1):
            data = img[addr - a.start: addr - a.start + SECTOR]
            for attempt in range(3):
                if bk.erase4k(addr) and bk.write4k(addr, data) and bk.read4k(addr) == data:
                    ok += 1; break
                bk.link(seconds=0.3)
            else:
                print(f"FAILED at {hex(addr)} after 3 attempts", flush=True)
                break
            if i % 32 == 0 or i == len(todo):
                print(time.strftime("%T"), f"{i}/{len(todo)} written+verified (last {hex(addr)})", flush=True)
    finally:
        if (orig & PROT_MASK) != 0:
            r = bk.set_protection(orig)
            print("protection restored" if r is not None else "WARNING: protection NOT restored", flush=True)
    print(f"done: {ok}/{len(todo)} sectors written and verified", flush=True)
    if ok != len(todo):
        sys.exit(1)


if __name__ == "__main__":
    main()
