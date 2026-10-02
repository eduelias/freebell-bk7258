"""Send one command to the doorbell's shell (UART0, 115200) and print the reply.

  python3 tools/shell.py wifi status
  python3 tools/shell.py wifi set MySSID MyPassword      # saved in the device's KV store
  python3 tools/shell.py hook show
  python3 tools/shell.py hook ring                       # simulate a ring -> HA webhook
  python3 tools/shell.py -w 5 pin watch 4                # wait up to 5 s for the reply

Heartbeat lines are filtered out. Use -i for an interactive session (Ctrl-C to leave).
"""
import glob, re, sys, time
import serial


def main():
    args = sys.argv[1:]
    wait = 1.0
    if args[:1] == ["-w"]:
        wait = float(args[1]); args = args[2:]
    s = serial.Serial(glob.glob("/dev/cu.usbserial-*")[0], 115200, timeout=0.05)

    def run(line, w):
        s.reset_input_buffer()
        s.write(line.encode() + b"\r\n")
        end = time.time() + w
        buf = b""
        while time.time() < end:
            buf += s.read(4096)
        for l in buf.decode("latin-1").replace("\r", "").split("\n"):
            l = re.sub(r"^\(\d+\):", "", l)   # strip the firmware's "(ms):" log prefix
            if l.strip() and "] alive " not in l:
                print(l)

    if args[:1] == ["-i"]:
        print("interactive; Ctrl-C to quit")
        try:
            while True:
                run(input("> "), 1.0)
        except (KeyboardInterrupt, EOFError):
            print()
        return
    run(" ".join(args), wait)


if __name__ == "__main__":
    main()
