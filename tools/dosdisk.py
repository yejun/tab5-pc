#!/usr/bin/env python3
"""Make and fill DOS hard disk images (one FAT partition) on the PC, for the Tab5 PC's SD card.

    tools/dosdisk.py new D.IMG [--mb 504] [--label DATA]   a blank, formatted disk (FAT16, up to 2047 MB)
    tools/dosdisk.py ls D.IMG [DIR]                        list a directory
    tools/dosdisk.py put D.IMG SRC... DEST                 copy files or directories in (DEST is a directory)
    tools/dosdisk.py get D.IMG SRC DEST                    copy a file out

DOS paths use / or \\, e.g. /GAMES. Names must suit DOS (8.3); long names are stored as long file names, which
plain DOS doesn't see. `put` doesn't replace files that are already there (pyfatfs can corrupt the directory doing
so), and checks the disk with fsck.fat afterwards if dosfstools is installed. Needs pyfatfs (requirements.txt): run
from anywhere, it uses the repository's .venv when there is one.
"""
import argparse
import os
import struct
import subprocess
import sys
import time
import warnings

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def use_venv():
    """Run this script again with the repository's .venv when pyfatfs isn't installed for this Python."""
    venv = os.path.join(ROOT, ".venv")
    python = os.path.join(venv, "bin", "python")
    if os.path.exists(python) and os.path.realpath(sys.prefix) != os.path.realpath(venv):
        os.execv(python, [python] + sys.argv)
    sys.exit("pyfatfs is missing: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt")


warnings.filterwarnings("ignore")   # pyfatfs pulls in fs, which warns about pkg_resources
try:
    from pyfatfs.PyFatFS import PyFatFS
except ImportError:
    use_venv()

HEADS, SECTORS = 16, 63     # the geometry tiny386's IDE disk reports (and DOS expects in the partition table)
START = SECTORS             # the partition starts on the second track, as DOS's FDISK does


def chs(lba):
    c, rest = divmod(lba, HEADS * SECTORS)
    h, s = divmod(rest, SECTORS)
    if c > 1023:            # beyond what CHS can say: the conventional maximum (DOS then uses the LBA values)
        c, h, s = 1023, HEADS - 1, SECTORS - 1
    return bytes([h, (s + 1) | (c >> 8) << 6, c & 0xFF])


def new(path, mb, label):
    if os.path.exists(path):
        sys.exit(f"{path} exists; delete it first")
    total = mb * 1024 * 1024 // 512
    total -= total % (HEADS * SECTORS)      # whole cylinders
    count = total - START
    # FAT16 (type 6), or FAT16 LBA (type 0x0E) past the 1024 cylinders CHS can address
    ptype = 0x06 if total <= 1024 * HEADS * SECTORS else 0x0E
    with open(path, "wb") as f:
        f.truncate(total * 512)
        mbr = bytearray(512)
        mbr[446:462] = bytes([0x80]) + chs(START) + bytes([ptype]) + chs(START + count - 1) + struct.pack("<II", START, count)
        mbr[510:512] = b"\x55\xaa"
        f.write(mbr)
        format_fat16(f, START, count, label)
    fsck(path, START * 512)
    print(f"{path}: {total * 512 >> 20} MB, FAT16, label {label.upper()}")


def format_fat16(f, start, count, label):
    """An empty FAT16 file system on the partition (count sectors from sector start), as DOS's FORMAT leaves it: 2 FATs,
    512 root entries, the cluster size DOS picks for the size, the volume label. The boot sector carries the geometry
    tiny386 reports (a DOS boot sector reads by cylinder/head/sector) and, as code, INT 18h (the next boot device);
    a system's SYS writes its own."""
    spc = next(n for limit, n in ((32680, 2), (262144, 4), (524288, 8), (1048576, 16), (2097152, 32), (1 << 32, 64))
               if count <= limit)
    reserved, root_n = 1, 512
    root_secs = root_n * 32 // 512
    fatsz = (count - reserved - root_secs + 256 * spc + 2 - 1) // (256 * spc + 2)   # (the FAT specification's)
    name = label[:11].upper().ljust(11).encode("ascii")
    b = bytearray(512)
    b[0:3] = b"\xeb\x3c\x90"
    b[3:11] = b"TAB5-PC "
    struct.pack_into("<HBHBHHBHHHII", b, 11, 512, spc, reserved, 2, root_n, count if count < 65536 else 0, 0xF8, fatsz,
                     SECTORS, HEADS, start, count if count >= 65536 else 0)
    struct.pack_into("<BBBI", b, 36, 0x80, 0, 0x29, int(time.time()) & 0xFFFFFFFF)
    b[43:62] = name + b"FAT16   "
    b[62:66] = b"\xcd\x18\xeb\xfe"   # int 18h; jmp $
    b[510:512] = b"\x55\xaa"
    base = start * 512
    f.seek(base)
    f.write(b)
    for i in range(2):   # each FAT: the media byte, then the end-of-chain mark
        f.seek(base + (reserved + i * fatsz) * 512)
        f.write(b"\xf8\xff\xff\xff")
    f.seek(base + (reserved + 2 * fatsz) * 512)   # the root directory: the volume label
    f.write(name + bytes([0x08]) + bytes(20))


def partition_offset(path):
    with open(path, "rb") as f:
        mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        sys.exit(f"{path}: no partition table")
    return struct.unpack("<I", mbr[446 + 8:446 + 12])[0] * 512


def dos(p):
    p = p.replace("\\", "/")
    return p if p.startswith("/") else "/" + p


def fsck(img, offset):
    """fsck.fat (read-only) on the partition at offset; exits if it finds anything."""
    part = img + ".part"
    with open(img, "rb") as src, open(part, "wb") as dst:
        src.seek(offset)
        while chunk := src.read(1 << 20):
            dst.write(chunk)
    try:
        r = subprocess.run(["fsck.fat", "-n", part], capture_output=True, text=True)
    except FileNotFoundError:
        print("(fsck.fat not found: the disk was not checked)")
        return
    finally:
        os.remove(part)
    if r.returncode != 0:
        sys.exit(f"{img}: fsck.fat found problems:\n{r.stdout}{r.stderr}")


def put(fs, src, dest):
    name = os.path.basename(os.path.normpath(src)).upper()
    target = dest.rstrip("/") + "/" + name
    if os.path.isdir(src):
        if not fs.exists(target):
            fs.makedir(target)
        for entry in sorted(os.listdir(src)):
            put(fs, os.path.join(src, entry), target)
    elif any(n.upper() == name for n in fs.listdir(dest)):
        print(f"  {target}: already there, skipped")
    else:
        with open(src, "rb") as f:
            fs.writebytes(target, f.read())
        print(f"  {target}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("new")
    a.add_argument("image")
    a.add_argument("--mb", type=int, default=504)
    a.add_argument("--label", default="DATA")
    a = sub.add_parser("ls")
    a.add_argument("image")
    a.add_argument("dir", nargs="?", default="/")
    a = sub.add_parser("put")
    a.add_argument("image")
    a.add_argument("paths", nargs="+")
    a = sub.add_parser("get")
    a.add_argument("image")
    a.add_argument("src")
    a.add_argument("dest")
    args = ap.parse_args()

    if args.cmd == "new":
        if not 8 <= args.mb <= 2047:
            sys.exit("--mb must be 8 to 2047 (FAT16)")
        return new(args.image, args.mb, args.label)
    fs = PyFatFS(args.image, offset=partition_offset(args.image), read_only=args.cmd in ("ls", "get"))
    try:
        if args.cmd == "ls":
            for name in sorted(fs.listdir(dos(args.dir))):
                p = dos(args.dir).rstrip("/") + "/" + name
                info = fs.getinfo(p, namespaces=["details"])
                print(f"{'<DIR>' if info.is_dir else info.size:>10}  {name}")
        elif args.cmd == "put":
            if len(args.paths) < 2:
                sys.exit("put needs SRC... DEST")
            dest = dos(args.paths[-1])
            if not fs.exists(dest):
                fs.makedirs(dest)
            for src in args.paths[:-1]:
                put(fs, src, dest)
        elif args.cmd == "get":
            with open(args.dest, "wb") as f:
                f.write(fs.readbytes(dos(args.src)))
    finally:
        fs.close()
    if args.cmd == "put":
        fsck(args.image, partition_offset(args.image))


if __name__ == "__main__":
    main()
