# Changelog

## 1.0.0 (2026-10-22)

The first public release, as source code (no firmware binaries or SD card images; see `docs/DISTRIBUTION.md`).

- A 386/486 PC on the M5Stack Tab5, built on tiny386 (pinned commit, with the patches in `vendor/patches`): VGA,
  PS/2 keyboard and mouse, Sound Blaster 16, AdLib, PC speaker, IDE hard disks, CD-ROM images, floppies, SeaBIOS.
- A fast interpreter and a translator to RISC-V code in front of tiny386's own CPU (see `docs/NOTES.md`).
- First start on an empty card: downloads FreeDOS 1.4 and runs its installer, or starts an installer CD image from
  the card; then sets the PC up to start from `C.IMG`. `tools/make_sdcard.py` prepares a card on a computer instead.
- The Tab5 Keyboard, USB keyboards and mice, and the touch screen as a touchpad.
- Copying files over Wi-Fi from a browser (the SD card and the PC's disks); `tools/upload.py` from a shell.
- The settings panel: Wi-Fi, brightness, volume, touchpad speed, pause and reset.
- The serial console: status, screenshots (`tools/shot.py`), commands.
