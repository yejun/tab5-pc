#!/usr/bin/env python3
"""After the link (tools/build.sh): the bytes of the firmware's .t386_xip section (the emulator's code, linked to run
from PSRAM: tools/xip.ld) go into the t386_xip_store array in flash, and the section itself becomes one without
contents, so that the image holds them once, where the firmware copies them from when it starts (xip.h). The ELF as
linked is kept beside it as <name>.full.elf, for disassembly and debuggers.

    tools/xip_patch.py build/prev3/tab5_386.ino.elf

Before the build, `xip_patch.py ld <the core's sections.ld> <out>` writes the linker script the link uses in its place:
the same with tools/xip.ld's output section before .flash.text (which would take the emulator's code otherwise).
"""
import shutil
import struct
import sys


def ld(stock, out):
    import os
    import re
    text = open(stock).read()
    piece = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "xip.ld")).read()
    new, n = re.subn(r"^(\s*\.flash\.text\s*:)", lambda m: piece + m.group(1), text, count=1, flags=re.M)
    if n != 1:
        raise SystemExit(f"xip_patch: no .flash.text in {stock}")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w").write(new)


def main():
    if sys.argv[1] == "ld":
        return ld(sys.argv[2], sys.argv[3])
    path = sys.argv[1]
    elf = bytearray(open(path, "rb").read())
    assert elf[:6] == b"\x7fELF\x01\x01", "a 32-bit little-endian ELF"
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    secs = [list(struct.unpack_from("<10I", elf, shoff + i * shentsize)) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    name = lambda off, base: bytes(elf[base + off:elf.index(0, base + off)]).decode()
    by_name = {name(s[0], stroff): (i, s) for i, s in enumerate(secs)}
    if ".t386_xip" not in by_name:
        raise SystemExit("xip_patch: no .t386_xip section (tools/xip.ld not used?)")
    xi, x = by_name[".t386_xip"]
    if x[1] == 8:
        return   # (done already: a link that was not repeated)
    shutil.copyfile(path, path[:-4] + ".full.elf")
    # the store: the symbols t386_xip_store and t386_xip_store_size (its sizeof)
    _, symtab = by_name[".symtab"]
    _, strtab = by_name[".strtab"]
    syms = {}
    for off in range(symtab[4], symtab[4] + symtab[5], 16):
        st_name, st_value, st_size, _, _, st_shndx = struct.unpack_from("<IIIBBH", elf, off)
        n = name(st_name, strtab[4])
        if n == "t386_xip_store":
            syms[n] = (st_value, st_size, st_shndx)
    if "t386_xip_store" not in syms:
        raise SystemExit("xip_patch: no t386_xip_store symbol")
    addr, size, shndx = syms["t386_xip_store"]
    if x[5] > size:
        raise SystemExit(f"xip_patch: the code is {x[5]} bytes, t386_xip_store only {size}: raise T386_XIP_MAX (xip.h)")
    home = secs[shndx]
    at = home[4] + addr - home[3]
    elf[at:at + x[5]] = elf[x[4]:x[4] + x[5]]
    struct.pack_into("<I", elf, shoff + xi * shentsize + 4, 8)   # SHT_NOBITS: esptool leaves it out of the image
    open(path, "wb").write(elf)
    print(f"xip_patch: {x[5]} bytes of code for PSRAM at {x[3]:#x}, stored at {addr:#x} ({size - x[5]} to spare)")


if __name__ == "__main__":
    main()
