#!/usr/bin/env python3
"""The firmware's uses of the tp register:  tp_check.py OBJDUMP ELF   (tools/build.sh; exit 1 if there are any)

Translated code keeps a host address in tp (t386_jit.h, R_GRP), so nothing in the firmware may read it as the
thread pointer it otherwise is. Allowed: tp saved to and restored from memory (an interrupt's entry and exit), and
the translator's own routine setting it (mv tp,a0).

The emulator's code in PSRAM (.t386_xip, tools/xip.ld) has its constants between its functions, and a disassembly
reads the ones without a symbol (strings, jump tables) as instructions, some of which name tp: there, only what
lies inside a function counts.
"""
import bisect
import re
import subprocess
import sys

od, elf = sys.argv[1:3]
funcs, lo, hi = [], 0, 0
for l in subprocess.run([od, "-t", elf], capture_output=True, text=True, check=True).stdout.splitlines():
    m = re.match(r"([0-9a-f]{8}) .{6}F \S+\t([0-9a-f]{8}) ", l)
    if m:
        funcs.append((int(m[1], 16), int(m[1], 16) + int(m[2], 16)))
    elif l.endswith(" _t386_xip_start"):
        lo = int(l[:8], 16)
    elif l.endswith(" _t386_xip_end"):
        hi = int(l[:8], 16)
funcs.sort()
starts = [f[0] for f in funcs]
uses_tp = re.compile(r"[\t ,(]tp([,)]|$)")
allowed = re.compile(r"(lw|sw)\ttp,\d+\((sp|t0)\)|mv\ttp,a0")
bad = {}
for l in subprocess.run([od, "-d", elf], capture_output=True, text=True, check=True).stdout.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\t\S+\s*\t(.*)", l)
    if not m or not uses_tp.search(m[2]) or allowed.fullmatch(m[2].strip()):
        continue
    a = int(m[1], 16)
    if lo <= a < hi:
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or a >= funcs[i][1]:
            continue   # a constant between two functions
    bad[m[2].strip()] = bad.get(m[2].strip(), 0) + 1
if bad:
    print("the firmware uses tp, which translated code changes (t386_jit.h, R_GRP):", file=sys.stderr)
    for k in sorted(bad):
        print(f"{bad[k]:7d} {k}", file=sys.stderr)
    sys.exit(1)
