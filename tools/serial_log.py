#!/usr/bin/env python3
"""Bounded, non-interactive serial capture.

`pio device monitor` needs a TTY on stdin and dies with termios.error without one
(platformio-core#5113), leaving an orphan holding the port. This reads for a fixed time
and always closes.

    tools/serial_log.py                 8 s at 115200
    tools/serial_log.py -s 20 -o        20 s, also written to logs/
    tools/serial_log.py --boot          74880, the ESP8266 boot ROM banner

Reading serial always restarts the board: macOS asserts DTR/RTS when the port is opened,
before any userspace setting applies, and clearing HUPCL does not prevent it. So there is
no way to observe a running board undisturbed over USB, and uptime always starts at zero.
"""

import argparse
import datetime
import glob
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_GLOBS = ("/dev/cu.usbserial*", "/dev/cu.wchusbserial*", "/dev/ttyUSB*")


def import_serial():
    """PlatformIO bundles pyserial; a system python3 usually does not. Ask pio where its
    interpreter is rather than hardcoding a path that changes on every upgrade."""
    try:
        import serial
        return serial
    except ImportError:
        pass

    if os.environ.get("_SERIAL_LOG_REEXEC"):
        sys.exit("error: no pyserial. fix: pip install pyserial")

    import json
    import subprocess
    try:
        out = subprocess.run(["pio", "system", "info", "--json-output"],
                             capture_output=True, text=True, timeout=60, check=True).stdout
        python_exe = json.loads(out)["python_exe"]["value"]
    except Exception:
        sys.exit("error: no pyserial and PlatformIO could not be queried. "
                 "fix: pip install pyserial")

    os.environ["_SERIAL_LOG_REEXEC"] = "1"
    os.execv(python_exe, [python_exe, os.path.abspath(__file__)] + sys.argv[1:])


def resolve_port(explicit):
    if explicit:
        return explicit
    found = [p for pattern in PORT_GLOBS for p in sorted(glob.glob(pattern))]
    if not found:
        sys.exit("error: no board found. Expected a CH340 at /dev/cu.usbserial-* "
                 "(VID:PID 1a86:7523); check `pio device list`.")
    if len(found) > 1:
        sys.exit("error: several candidate ports, pass --port:\n  " + "\n  ".join(found))
    return found[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", help="serial device (default: autodetect)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--boot", action="store_true", help="--baud 74880, the boot ROM rate")
    ap.add_argument("-s", "--seconds", type=float, default=8.0)
    ap.add_argument("-t", "--timestamp", action="store_true")
    ap.add_argument("-o", "--out", nargs="?", const="", metavar="FILE",
                    help="also write to FILE, or a timestamped file under logs/")
    args = ap.parse_args()

    serial = import_serial()
    baud = 74880 if args.boot else args.baud
    port = resolve_port(args.port)

    sink = None
    if args.out is not None:
        path = args.out
        if not path:
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            os.makedirs(os.path.join(REPO, "logs"), exist_ok=True)
            path = os.path.join(REPO, "logs", f"serial-{stamp}-{baud}.log")
        sink = open(path, "w", encoding="utf-8")
        print(f"# logging to {path}", file=sys.stderr)

    print(f"# {port} @ {baud}", file=sys.stderr)

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, baud, 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()

    # Opening already reset the board, but at an uncontrolled moment. Pulse EN again so
    # the banner lands after we are reading rather than during open. RTS drives EN, DTR
    # drives GPIO0; DTR stays low so the chip boots to flash, not the serial bootloader.
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False

    start = time.time()
    deadline = start + args.seconds
    pending = b""

    def emit(raw):
        # The first second after a reset is the 74880 boot ROM banner seen at the wrong
        # rate. That garbage is expected, so decode leniently.
        text = raw.decode("utf-8", errors="replace").rstrip("\r")
        if args.timestamp:
            text = f"[{time.time() - start:7.3f}] {text}"
        print(text, flush=True)
        if sink:
            sink.write(text + "\n")

    try:
        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            pending += chunk
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                emit(raw)
    finally:
        if pending:
            emit(pending)
        ser.close()
        if sink:
            sink.close()


if __name__ == "__main__":
    main()
