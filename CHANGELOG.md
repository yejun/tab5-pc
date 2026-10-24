# Changelog

## Unreleased

- The PC's network card (NE2000, port 300h, IRQ 9) is connected while Wi-Fi is on: a network of its own inside the
  Tab5 (192.168.77.0/24, the Tab5 at .1 with a DHCP server), routed to the Wi-Fi network with address translation;
  ports 2222, 2323 and 8080 of the Tab5 lead to the PC's 22, 23 and 80. Serial `net` shows the card's state.
- One lock for the SD card: the PC's disks (raw access) and FatFs (the web page, the settings panel) each had their
  own, and a command sent while the card was busy with the other's could time out, so a web download could end
  early. Downloads that fail now close the connection (the browser reports an error, not a shorter file).
- The translator: more instructions translated (SHLD/SHRD by CL and in memory, BSF/BSR, BT/BTS/BTR/BTC in memory
  with the bit number in a register, BSWAP, RDTSC, CMPXCHG, XADD, FWAIT); a bigger table of translated blocks; in
  protected mode with DS, ES and SS based at 0 (flat), their bases aren't added to every address. The fast
  interpreter computes the carry flag inline.
- tiny386 fixes (`vendor/patches`): CMPXCHG set the flags of destination - accumulator instead of accumulator -
  destination (0014); 64-bit integers and 80-bit values copied through the FPU (FILD/FISTP, FLD/FSTP) lost bits
  beyond a double's 53 (0015).
- The translator's description (README, `docs/NOTES.md`): it handles paged protected mode too.
- The PC's picture at 4:3, as a monitor showed every mode (the default): text, 320 x 200 and 640 x 350 graphics fill
  the panel's height like 640 x 480 (at most 960 x 720). A switch in the settings panel, or serial `aspect 43|square`,
  goes back to square pixels. Fixed with it: the last lines of a picture whose height isn't a multiple of 16
  (640 x 350) weren't redrawn, and the black around a picture was cleared on the wrong side upside down.
- tiny386 at upstream 1a8f1e0 (was d81e225), which has the task-switch and timer-mode fixes (patch 0011 and
  `t386_pit_fix.h` dropped) and VGA, PS/2, ATAPI and floppy fixes; patches 0002, 0004 and 0007 rebased onto it.
- Disk images that lie in pieces on the card (copied onto a card in use) are read and written as card sectors too,
  through a list of their pieces made at startup, instead of through FatFs, which walked the file's cluster chain on
  every backward seek. Reading a 3 MB file from an image in 590 pieces took 2.6 s instead of 85 s.
- Video memory in the VGA's other write modes is written without tiny386's callbacks: write mode 0 with rotation,
  set/reset, a logical operation or a bit mask, and write modes 2 and 3, by translated code itself, and read mode 1
  (colour compare) by the fast path. Twenty fills of the 640 x 480 x 16 screen with set/reset or write modes 2 and 3
  take about 6-7 BIOS ticks instead of 16-17. Writes go nowhere while the misc output register's RAM enable bit is
  clear, as in vga.c.

## 1.0.0 (2026-10-22)

The first public release: the source code, and firmware images with their notices and corresponding source (see
`docs/DISTRIBUTION.md`). No SD card images.

- A 386/486 PC on the M5Stack Tab5, built on tiny386 (pinned commit, with the patches in `vendor/patches`): VGA,
  PS/2 keyboard and mouse, Sound Blaster 16, AdLib, PC speaker, IDE hard disks, CD-ROM images, floppies, SeaBIOS.
- A fast interpreter and a translator to RISC-V code in front of tiny386's own CPU (see `docs/NOTES.md`).
- First start on an empty card: downloads FreeDOS 1.4 and runs its installer, or starts an installer CD image from
  the card; then sets the PC up to start from `C.IMG`. `tools/make_sdcard.py` prepares a card on a computer instead.
- The Tab5 Keyboard, USB keyboards and mice, and the touch screen as a touchpad.
- Copying files over Wi-Fi from a browser (the SD card and the PC's disks); `tools/upload.py` from a shell.
- The settings panel: Wi-Fi, brightness, volume, touchpad speed, pause and reset.
- The serial console: status, screenshots (`tools/shot.py`), commands.
