"""Boot the firmware from ROM download mode without touching the hardware.

After bkwrite.py finishes, the chip is still parked in the BootROM (the link-check spam keeps it
there), and with the USB charger connected, pulling the battery does NOT power-cycle the board.
The ROM's CMD_Reboot (0x0E, payload 0xA5) resets the chip so it boots the flashed image.

  bkreboot.py            reboot, then print 30 s of boot log
  bkreboot.py --quiet    reboot only
  --port DEV selects the UART adapter (default: SERIAL_PORT or the first USB serial device).
"""
import argparse, sys, time
from bkread import BK

REBOOT = bytes.fromhex("01e0fc020ea5")   # 01 E0 FC <len> CMD_Reboot 0xA5


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", action="store_true", help="reboot only, do not print the boot log")
    ap.add_argument("--port", help="UART adapter")
    a = ap.parse_args()
    bk = BK(port=a.port)
    if not bk.link(seconds=5):
        sys.exit("chip is not in ROM download mode (nothing to reboot from)")
    bk.s.reset_input_buffer()
    bk.s.write(REBOOT)
    print(time.strftime("%T"), "reboot sent")
    if a.quiet:
        return
    bk.s.timeout = 0.2
    end = time.time() + 30
    buf = b""
    while time.time() < end:
        try:
            buf += bk.s.read(4096)
        except Exception:
            bk.reopen(); bk.s.timeout = 0.2
    sys.stdout.write(buf.decode("latin-1"))


if __name__ == "__main__":
    main()
