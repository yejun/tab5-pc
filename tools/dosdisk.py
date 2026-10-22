#!/usr/bin/env python3
"""Make and fill DOS hard disk images (one FAT partition) on the PC, for the Tab5 PC's SD card.

    tools/dosdisk.py new D.IMG [--mb 504] [--label DATA]   a blank, formatted disk (FAT16, up to 2047 MB)
    tools/dosdisk.py ls D.IMG [DIR]                        list a directory
    tools/dosdisk.py put D.IMG SRC... DEST                 copy files or directories in (DEST is a directory)
    tools/dosdisk.py get D.IMG SRC DEST                    copy a file out

DOS paths use / or \\, e.g. /GAMES. Names must suit DOS (8.3); long names are stored as long file names, which
plain DOS doesn't see. `put` doesn't replace files that are already there (pyfatfs can corrupt the directory doing
so), and checks the disk with fsck.fat afterwards. Needs dosfstools (mkfs.fat, fsck.fat) and pyfatfs (requirements.txt).
"""
import argparse
import os
import struct
import subprocess
import sys
import warnings

warnings.filterwarnings("ignore")   # pyfatfs pulls in fs, which warns about pkg_resources
from pyfatfs.PyFatFS import PyFatFS  # noqa: E402

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
    # the geometry in the boot sector as tiny386 reports it: a DOS boot sector reads by cylinder/head/sector
    subprocess.run(["mkfs.fat", "-F", "16", "--offset", str(START), "-h", str(START), "-S", "512",
                    "-g", f"{HEADS}/{SECTORS}", "-n", label[:11].upper(), path, str(count // 2)],
                   check=True, stdout=subprocess.DEVNULL)
    print(f"{path}: {total * 512 >> 20} MB, FAT16, label {label.upper()}")


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
