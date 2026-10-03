"""Send one command to the doorbell's shell (UART0, 115200) and print the reply.

  python3 tools/shell.py wifi status
  python3 tools/shell.py wifi set MySSID MyPassword      # saved in the device's KV store
  python3 tools/shell.py hook show
  python3 tools/shell.py hook ring                       # simulate a ring -> HA webhook
  python3 tools/shell.py -w 5 pin watch 4                # wait up to 5 s for the reply

Heartbeat lines are filtered out. Use -i for an interactive session (Ctrl-C to leave).
--port DEV picks the UART adapter (default: SERIAL_PORT or the first USB serial device).
"""
import re, sys, time
import serial
import dbenv


def main():
    args = sys.argv[1:]
    wait, port, interactive = 1.0, None, False
    while args and args[0] in ("-w", "-i", "--port") or (args and args[0].startswith("--port=")):
        if args[0] == "-w":
            wait = float(args[1]); args = args[2:]
        elif args[0] == "-i":
            interactive = True; args = args[1:]
        elif args[0] == "--port":
            port = args[1]; args = args[2:]
        else:
            port = args[0].split("=", 1)[1]; args = args[1:]
    if not args and not interactive:
        sys.exit(__doc__)
    s = serial.Serial(dbenv.serial_port(port), 115200, timeout=0.05)

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

    if interactive:
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
