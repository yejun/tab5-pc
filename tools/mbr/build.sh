#!/usr/bin/env bash
# Assemble mbr.S (GNU binutils) into firmware/tab5_386/mbr_code.h: the boot code of a new C.IMG's first sector.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
as --32 -o "$tmp/mbr.o" mbr.S
ld -m elf_i386 -Ttext=0x600 -e _start -o "$tmp/mbr.elf" "$tmp/mbr.o"
objcopy -O binary -j .text "$tmp/mbr.elf" "$tmp/mbr.bin"
size=$(stat -c %s "$tmp/mbr.bin")
(( size == 512 )) || { echo "mbr.bin is $size bytes, not 512" >&2; exit 1; }
{
  echo "// The master boot record of a new C.IMG (tools/mbr/mbr.S, made by tools/mbr/build.sh): its first 446 bytes, the"
  echo "// boot code (the partition table and the signature are written by fd_install.h)."
  echo "static const uint8_t MBR_CODE[446] = {"
  head -c 446 "$tmp/mbr.bin" | od -An -v -tx1 | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1, /g; s/^/   /; s/, $/,/'
  echo "};"
} > ../../firmware/tab5_386/mbr_code.h
echo "made firmware/tab5_386/mbr_code.h ($(head -c 446 "$tmp/mbr.bin" | tr -d '\0' | wc -c) bytes of code)"
