#!/usr/bin/env python3
"""Talk to the Tab5 386 over USB serial: print what it says, optionally sending commands first.

    tools/console.py [--port PORT] [--secs N] [--reset] [CMD ...]
"""
import argparse
import time

import serial
from serial.tools import list_ports


def open_port(port):
    if not port:
        ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
        if not ports:
            raise SystemExit("no Tab5 found on USB; pass --port")
        port = ports[0]
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = True   # with both low, opening the port resets the P4
    s.open()
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmds", nargs="*")
    ap.add_argument("--port")
    ap.add_argument("--secs", type=float, default=2.0, help="how long to listen after the last command")
    ap.add_argument("--reset", action="store_true", help="restart the Tab5 first (RTS)")
    args = ap.parse_args()
    s = open_port(args.port)
    if args.reset:
        s.dtr = False   # RTS without DTR: the chip's reset
        time.sleep(0.1)
        s.close()
        time.sleep(1.5)
        s = open_port(args.port)
    for c in args.cmds:
        s.write(c.encode() + b"\n")
        time.sleep(0.3)
    end = time.monotonic() + args.secs
    while time.monotonic() < end:
        data = s.read(4096)
        if data:
            print(data.decode(errors="replace"), end="", flush=True)


if __name__ == "__main__":
    main()
