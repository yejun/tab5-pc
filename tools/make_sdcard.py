#!/usr/bin/env python3
"""Make the SD card's disk images in sdcard/: C.IMG, FreeDOS 1.4 (the LiteUSB live disk, changed to start at the
C:\\> prompt instead of the installer, with all of its packages installed: the mouse driver, DEBUG, UNZIP, CWSDPMI, ...),
D.IMG, a blank 504 MB FAT16 disk for programs and games (with D:\\TEST\\BEEP.COM, a speaker test), and tiny386.ini,
the machine's settings for them. Copy sdcard/C.IMG, sdcard/D.IMG and sdcard/tiny386.ini to the root of the card.

    tools/make_sdcard.py [--force]
"""
import argparse
import io
import os
import subprocess
import sys
import urllib.request
import warnings
import zipfile

warnings.filterwarnings("ignore")
from pyfatfs.PyFatFS import PyFatFS  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dosdisk import fsck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL = "https://download.freedos.org/1.4/FD14-LiteUSB.zip"
CACHE = os.path.join(ROOT, "images", "FD14-LiteUSB.zip")


# BEEP.COM: a 1 kHz tone for half a second on the PC speaker (PIT channel 2), hand-assembled:
#   mov al,0b6h / out 43h,al / mov ax,1193 / out 42h,al / mov al,ah / out 42h,al   ; channel 2, square wave, 1 kHz
#   in al,61h / or al,3 / out 61h,al                                              ; speaker on
#   mov ah,86h / mov cx,7 / mov dx,0a120h / int 15h                               ; wait 500000 us
#   in al,61h / and al,0fch / out 61h,al / ret                                    ; speaker off, exit
BEEP_COM = bytes.fromhex("b0b6e643b8a904e64288e0e642e4610c03e661b486b90700ba20a1cd15e46124fce661c3")

# What the PC has: the mouse driver (PS/2), and where programs find the Sound Blaster 16 (sb16.c, pc.c).
AUTOEXEC_ADD = """
rem Tab5 386: the PS/2 mouse (the touch screen) and the Sound Blaster 16
SET BLASTER=A220 I5 D1 H5 T6
CTMOUSE
"""

# The machine's settings (the firmware reads /sdcard/tiny386.ini)
TINY386_INI = """\
; Tab5 386 machine settings. Copy this file and the disk images to the root of the SD card (FAT32 or exFAT).
; /sdcard is the root of the card.

[pc]
; hard disks hda hdb hdc hdd, CD-ROMs cda cdb cdc cdd (an .iso), floppies fda fdb (1.44 MB etc. images)
hda = /sdcard/C.IMG
hdb = /sdcard/D.IMG
;fda = /sdcard/floppy.img
;cdc = /sdcard/cd.iso
mem_size = 16M
vga_mem_size = 2M
; 1: the hard disks' geometry in the CMOS (some systems want it, others not)
fill_cmos = 1
; the BIOS is built into the firmware; to use other images, put them on the card:
;bios = /sdcard/bios.bin
;vga_bios = /sdcard/vgabios.bin

[cpu]
; 3/4/5/6 = 386/486/586/686
gen = 4
; 1 = x87 maths coprocessor (some programs need it)
fpu = 1
"""


def install_packages(fs):
    """Unpack every package zip in /PACKAGES into /FREEDOS (without sources). The zips are read first and removed
    before unpacking, for the room. Files the live disk already has (the same release) are kept: replacing a file
    with pyfatfs can leave two directory entries sharing its clusters."""
    files, count = {}, 0
    for group in sorted(fs.listdir("/PACKAGES")):
        gdir = f"/PACKAGES/{group}"
        if not fs.isdir(gdir):
            continue
        for name in sorted(fs.listdir(gdir)):
            if not name.lower().endswith(".zip"):
                continue
            z = zipfile.ZipFile(io.BytesIO(fs.readbytes(f"{gdir}/{name}")))
            for info in z.infolist():
                parts = info.filename.upper().strip("/").split("/")
                if parts[0] != "SOURCE" and not info.is_dir():
                    files["/FREEDOS/" + "/".join(parts)] = z.read(info)
            count += 1
    fs.removetree("/PACKAGES")
    for path, data in sorted(files.items()):
        folder, base = path.rsplit("/", 1)
        fs.makedirs(folder, recreate=True)
        if any(n.upper() == base for n in fs.listdir(folder)):
            continue
        fs.writebytes(path, data)
    for folder in {p.rsplit("/", 1)[0] for p in files}:
        names = [n.upper() for n in fs.listdir(folder)]
        dupes = {n for n in names if names.count(n) > 1}
        if dupes:
            sys.exit(f"duplicate names in {folder}: {sorted(dupes)}")
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="replace existing images (their contents are lost)")
    args = ap.parse_args()
    c_img, d_img = (os.path.join(ROOT, "sdcard", n) for n in ("C.IMG", "D.IMG"))
    for p in (c_img, d_img):
        if os.path.exists(p) and not args.force:
            sys.exit(f"{p} exists (use --force to replace it)")
    if not os.path.exists(CACHE):
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        print(f"downloading {URL}")
        urllib.request.urlretrieve(URL, CACHE)
    with zipfile.ZipFile(CACHE) as z, open(c_img, "wb") as out:
        out.write(z.read("FD14LITE.img"))
    fs = PyFatFS(c_img, offset=63 * 512)
    auto = fs.readtext("/FDAUTO.BAT", encoding="cp437")
    # the live disk's paths have no drive letter (it doesn't know its own): from D:, PATH would point at D:\FREEDOS
    auto = auto.replace("SET DOSDIR=\\FREEDOS", "SET DOSDIR=C:\\FREEDOS")
    auto = auto.replace("call CDROM.BAT", "rem call CDROM.BAT  (no CD-ROM on the Tab5 386 by default)")
    auto = auto.replace("if exist %dosdir%\\bin\\cdrom.bat call %dosdir%\\bin\\cdrom.bat display",
                        "rem if exist %dosdir%\\bin\\cdrom.bat call %dosdir%\\bin\\cdrom.bat display")
    fs.writetext("/FDAUTO.BAT", auto.replace("\r\n", "\n").replace("\n", "\r\n"), encoding="cp437")
    fs.move("/SETUP.BAT", "/SETUP.OLD")   # FDAUTO.BAT starts the installer only while SETUP.BAT exists
    n = install_packages(fs)
    auto = fs.readtext("/FDAUTO.BAT", encoding="cp437").replace("\r\n", "\n")
    auto = auto.replace("\n:Done\n", "\n:Done\n" + AUTOEXEC_ADD.lstrip("\n"), 1)
    fs.writetext("/FDAUTO.BAT", auto.replace("\n", "\r\n"), encoding="cp437")
    free = fs.fs.bytes_per_cluster * len([c for c in fs.fs.fat if c == 0]) if hasattr(fs.fs, "fat") else 0
    fs.close()
    fsck(c_img, 63 * 512)
    print(f"{c_img}: FreeDOS 1.4, 32 MB, {n} packages installed" + (f", {free >> 20} MB free" if free else ""),
          flush=True)
    if os.path.exists(d_img):
        os.remove(d_img)
    tool = os.path.join(ROOT, "tools", "dosdisk.py")
    subprocess.run([sys.executable, tool, "new", d_img], check=True)
    readme = os.path.join(ROOT, "build", "README.TXT")
    os.makedirs(os.path.dirname(readme), exist_ok=True)
    with open(readme, "w", newline="\r\n") as f:
        f.write("Your D: drive. Put programs and games here, from the PC:\n"
                "  tools/dosdisk.py put sdcard/D.IMG <files or folders> /GAMES\n")
    subprocess.run([sys.executable, tool, "put", d_img, readme, "/"], check=True, stdout=subprocess.DEVNULL)
    beep = os.path.join(ROOT, "build", "BEEP.COM")
    with open(beep, "wb") as f:
        f.write(BEEP_COM)
    subprocess.run([sys.executable, tool, "put", d_img, beep, "/TEST"], check=True, stdout=subprocess.DEVNULL)
    ini = os.path.join(ROOT, "sdcard", "tiny386.ini")
    with open(ini, "w", newline="\r\n") as f:
        f.write(TINY386_INI)
    print(f"{ini}: hda = C.IMG, hdb = D.IMG\nCopy C.IMG, D.IMG and tiny386.ini from sdcard/ to the root of the card.")


if __name__ == "__main__":
    main()
