#!/usr/bin/env python3
"""Copy files or folders to the Tab5 386 over WiFi (its web server), in the 256 KB chunks it takes.

    tools/upload.py [--resume] HOST VOL DEST SRC...   e.g. tools/upload.py 192.168.1.50 hdb /APPS ~/dos/MYPROG
    VOL: hda..hdd (the PC's disks: the PC pauses, and restarts when done) or sd (the card)
    --resume: a file already partly there goes on from where it stopped (a big disk image, say)
    tools/upload.py HOST --done              close the PC's disk now (the PC restarts if it changed)
A chunk that gets no answer is sent again (each goes to its own offset, so that is harmless).
"""
import json
import os
import sys
import time
import urllib.parse
import urllib.request

CHUNK = 256 << 10   # the Tab5 takes up to 256 KB at a time (web.h UPLOAD_CHUNK)


def call(host, method, path, params, data=None, tries=5):
    url = f"http://{host}{path}?{urllib.parse.urlencode(params)}"
    for attempt in range(tries):
        req = urllib.request.Request(url, data=data, method=method)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            sys.exit(f"{method} {path} {params.get('path', '')}: {e.read().decode(errors='replace')}")
        except (urllib.error.URLError, OSError) as e:   # no answer: again, after a pause
            if attempt == tries - 1:
                raise
            print(f"  ({method} {params.get('path', '')} at {params.get('offset', '')}: {e}; again)", flush=True)
            time.sleep(5)


def remote_size(host, vol, dest):   # the size of a file there, 0 if none
    parent, name = dest.rsplit("/", 1)
    try:
        for e in json.loads(call(host, "GET", "/api/ls", {"vol": vol, "path": parent or "/"})):
            if e["n"].upper() == name.upper() and not e["d"]:
                return e["s"]
    except SystemExit:
        pass
    return 0


def put_file(host, vol, dest, src, resume=False):
    size = os.path.getsize(src)
    off = remote_size(host, vol, dest) // CHUNK * CHUNK if resume else 0
    if off >= size:
        off = 0 if size else off
    if off:
        print(f"  {dest}: on from {off} bytes", flush=True)
    with open(src, "rb") as f:
        f.seek(off)
        while True:
            data = f.read(CHUNK)
            params = {"vol": vol, "path": dest, "offset": off}
            if off == 0:
                params["size"] = size   # (a big file for the card goes where it fits in one piece)
            call(host, "PUT", "/api/file", params, data)
            off += len(data)
            if size >= 16 << 20 and off % (16 << 20) == 0:
                print(f"  {dest}: {off >> 20} of {size >> 20} MB", flush=True)
            if off >= size:
                break
    print(f"  {dest} ({size} bytes)", flush=True)


def main():
    if len(sys.argv) == 3 and sys.argv[2] == "--done":
        call(sys.argv[1], "POST", "/api/pc", {"do": "done"})
        return
    args = sys.argv[1:]
    resume = "--resume" in args
    args = [a for a in args if a != "--resume"]
    if len(args) < 4:
        sys.exit(__doc__)
    host, vol, dest = args[:3]
    dest = "/" + dest.strip("/")
    for src in args[3:]:
        base = os.path.basename(os.path.normpath(src))
        if os.path.isdir(src):
            for root, _, files in os.walk(src):
                for name in sorted(files):
                    rel = os.path.relpath(os.path.join(root, name), src).replace(os.sep, "/")
                    put_file(host, vol, f"{dest.rstrip('/')}/{base}/{rel}", os.path.join(root, name), resume)
        else:
            put_file(host, vol, f"{dest.rstrip('/')}/{base}", src, resume)
    if vol.startswith("hd"):
        print("the PC's disk stays open (PC paused) for 30 s after the last copy, or until: upload.py HOST --done")


if __name__ == "__main__":
    main()
