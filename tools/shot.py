#!/usr/bin/env python3
"""Save the emulated PC's screen (tiny386's 720 x 480 frame buffer, read over USB serial) as a PNG, or with
--panel, what the Tab5's panel shows (its portrait frame buffer, turned to landscape as rotation --rot sees it).

    tools/shot.py out.png [--port PORT] [--panel [--rot 1|3]]
"""
import argparse
import time

import numpy as np
from PIL import Image

from console import open_port


def mix(data):
    """firmware's t386_mix (FNV-like, 4 bytes at a time) over data"""
    h = 0x811C9DC5
    w = np.frombuffer(data[: len(data) // 4 * 4], "<u4")
    for x in w.tolist():
        h = ((h ^ x) * 0x9E3779B1) & 0xFFFFFFFF
        h ^= h >> 15
    for b in data[len(data) // 4 * 4 :]:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--port")
    ap.add_argument("--panel", action="store_true")
    ap.add_argument("--rot", type=int, default=3, choices=(1, 3), help="the Tab5's rotation (3: docked in the keyboard)")
    args = ap.parse_args()
    s = open_port(args.port)
    s.timeout = 2
    for attempt in range(3):
        rgb = grab(s, args)
        if rgb is not None:
            Image.fromarray(np.ascontiguousarray(rgb)).save(args.out)
            print(f"saved {args.out}")
            return
        print("the picture came over garbled (checksum differs), again")
    raise SystemExit("no picture came over whole")


def grab(s, args):
    """one picture as an RGB array, or None if it came over garbled"""
    s.reset_input_buffer()
    s.write(b"P\n" if args.panel else b"F\n")
    end = time.monotonic() + 5
    while time.monotonic() < end:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("FB "):
            break
    else:
        raise SystemExit("no frame buffer header from the Tab5")
    w, h, n = map(int, line.split()[1:])
    data, end = b"", time.monotonic() + 30
    while len(data) < n and time.monotonic() < end:
        data += s.read(n - len(data))
    if len(data) != n:
        raise SystemExit(f"short read: {len(data)} of {n} bytes")
    end = time.monotonic() + 5
    while time.monotonic() < end:   # the Tab5's checksum of what it sent (t386_mix)
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("SUM "):
            if int(line[4:], 16) != mix(data):
                print(s.readline().decode(errors="replace").strip())
                return None
            break
    px = np.frombuffer(data, "<u2").reshape(h, w)
    rgb = np.stack([(px >> 11) << 3, ((px >> 5) & 63) << 2, (px & 31) << 3], axis=2).astype(np.uint8)
    if args.panel:   # rotation 1 maps landscape (x, y) to panel (719 - y, x); rotation 3 to (y, 1279 - x)
        rgb = np.rot90(rgb, 1) if args.rot == 1 else np.rot90(rgb, -1)
    return rgb


if __name__ == "__main__":
    main()
