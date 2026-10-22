#!/usr/bin/env bash
# Build the BIOS images (vendor/bios.bin, vendor/vgabios.bin) from source: SeaBIOS at the commit in vendor/bios.lock
# with tiny386's seabios/patch and seabios/config (from vendor/tiny386, tools/fetch_vendor.sh), as tiny386's
# scripts/build.sh does. Needs git, make, python3 and a gcc that can build 32-bit x86 code (gcc -m32).
#
# The result is built from the same source as the pinned images, but with this computer's compiler, so its bytes
# differ from theirs (tools/check_bios.py reports that). SeaBIOS writes the build's date and this computer's host name
# into its version string, which the firmware prints at start.
set -euo pipefail
cd "$(dirname "$0")/../vendor"
[[ -f tiny386/seabios/patch ]] || { echo "run tools/fetch_vendor.sh first" >&2; exit 1; }
SEABIOS=$(awk '$1 == "seabios" { print $2 }' bios.lock)
rm -rf seabios
git clone -q https://github.com/coreboot/seabios.git seabios
git -C seabios checkout -q "$SEABIOS"
(cd seabios && patch -s -p1 < ../tiny386/seabios/patch && cp ../tiny386/seabios/config .config &&
 make -s oldconfig >/dev/null && make -s -j"$(nproc 2>/dev/null || echo 4)")
cp seabios/out/bios.bin seabios/out/vgabios.bin .
echo "built bios.bin, vgabios.bin from SeaBIOS $SEABIOS"
