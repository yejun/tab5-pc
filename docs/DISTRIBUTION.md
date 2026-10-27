# Before distributing binaries

This repository is published as source code: our code under MIT, plus the third-party material listed in
`THIRD_PARTY_NOTICES.md`. Compiled firmware and ready-made SD card images are different: they contain third-party
code that this repository only fetches, some of it under the LGPL and GPL. This page lists what goes with them. It
is not a legal review.

## Compiled firmware

A firmware image contains tiny386, FMOPL, SeaBIOS, ESP-Hosted, the Arduino core, ESP-IDF's libraries, M5Unified,
M5GFX and LVGL (with its fonts), as listed in `THIRD_PARTY_NOTICES.md`. Each released image is published on the
GitHub release page together with:

1. **`tab5-pc-<version>-NOTICES.txt`**: the notices and full license texts of everything in the image (MIT, BSD,
   Apache-2.0 with ESP-IDF's component list, LGPL-2.1, LGPL-3.0 with GPL-3.0, OFL-1.1, Font Awesome's terms).
2. **`tab5-pc-<version>-source.tar.gz`**: the corresponding source: this repository at the release tag; tiny386 and
   ESP-Hosted as fetched and patched (FMOPL among them); the BIOS images and SeaBIOS's source at the pinned commit with
   tiny386's patch and config; the Arduino core's source as built (the board package's `cores/` and `libraries/`,
   m5stack:esp32 3.3.9). M5Unified, M5GFX and LVGL (MIT) are named by version.
3. **The rebuild route** for the LGPL parts: the README's "Build and flash" with the versions above. Change
   `vendor/tiny386/fmopl.c`, SeaBIOS (`tools/build_seabios.sh`) or the board package's core, rebuild, flash.
   ESP-IDF's libraries come precompiled in the board package (ESP-IDF v5.5.4, Apache-2.0 and the component licenses
   in the notices).
4. **The BIOS images, pinned**: the tested images (SHA-256 in `vendor/bios.lock`) are in the source archive, not
   fetched again from tiny386's continuous release.

Still open:

- **`vgafont.c`**: tiny386's VGA text font has no notice of its own; it is distributed as part of tiny386.
- **Chip revisions**: releases carry the `prev3` image only (the Tab5's ESP32-P4, v1.x). `CHIP=postv3` builds for
  v3.x, which no Tab5 has yet; such a build has not been tested on a device.
- **The ESP32-C6's firmware** (the Tab5's Wi-Fi chip) is not part of this firmware and is not changed by it.

## SD card images (`tools/make_sdcard.py`)

`tools/make_sdcard.py` builds `C.IMG` from FreeDOS 1.4's LiteUSB disk and installs the binary packages on it. It
leaves out the SOURCE packages. Distributing such an image means distributing FreeDOS (kernel, shell and many GPL
programs) in binary form, which is a separate deliverable from this project's source. Before offering one:

- Ship the GPL source for every GPL package in the image, or a written offer for it. FreeDOS publishes its sources
  (the release's source packages and the FreeDOS repositories). The simplest compliant route is to publish the
  matching FreeDOS 1.4 source packages next to the image, at the same place and for as long as the image.
- Include each package's license and documentation as installed (the packages' `APPINFO` and `DOC` folders), and
  say in a README which FreeDOS release and which packages the image contains and what was changed
  (`FDAUTO.BAT`, `SETUP.BAT` renamed, our `BEEP.COM` on `D:`).
- Record the checksum of the `FD14-LiteUSB.zip` it was made from.
- Offer the image under the FreeDOS name only as FreeDOS allows, and say that it is not an official FreeDOS release.
- Never include proprietary operating systems, commercial programs or games, or anything else without a license that
  allows redistribution, not even as an example. Users supply their own software.

The Tab5 itself downloads FreeDOS from freedos.org on the user's request. That is not a redistribution by this
project.
