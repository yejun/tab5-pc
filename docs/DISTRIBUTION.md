# Before distributing binaries

This repository is meant to be published as source code: our code under MIT, plus the third-party material listed in
`THIRD_PARTY_NOTICES.md`. Compiled firmware and ready-made SD card images are different: they contain third-party
code that this repository only fetches, some of it under the LGPL and GPL. This page lists what is still needed
before either is offered for download. None of it has been done yet, and none of it is a legal review.

## Compiled firmware (`tab5_386.ino.merged.bin` and the like)

A firmware image contains tiny386, FMOPL, SeaBIOS, ESP-Hosted, the Arduino core, ESP-IDF's libraries, M5Unified,
M5GFX and LVGL (with its fonts), as listed in `THIRD_PARTY_NOTICES.md`. Before distributing one:

1. **Notices and license texts for everything in the image**, shipped with it. Besides the texts in `LICENSES/`:
   tiny386's MIT files (Bellard, Karpov, Henke), inih's BSD-3-Clause, the LGPL-2.1 (FMOPL, the Arduino core), the
   LGPL-3.0 together with the GPL-3.0 it builds on (SeaBIOS), and the notices of ESP-IDF's components, M5Unified,
   M5GFX and the parts it includes, LVGL, Montserrat (SIL OFL 1.1) and Font Awesome Free. ESP-IDF's and the Arduino
   core's own license lists are a starting point. Linker map files (`build/*/tab5_386.ino.map`) show what an image
   actually contains, which may be less than the board package provides.
2. **The corresponding source** for the LGPL parts, for the exact build: this repository at the release's commit,
   tiny386 at `vendor/tiny386.commit`, ESP-Hosted at `vendor/esp-hosted.commit`, the BIOS source in
   `vendor/bios.lock`, and the board package and library versions (m5stack:esp32 3.3.9, M5Unified 0.2.25, M5GFX
   0.2.33, lvgl 9.6.0). Offer it next to the binary, or with a written offer as the LGPL describes. Depending on
   upstream download sites alone is fragile: those versions can disappear.
3. **A working way to rebuild and relink with modified LGPL parts.** Statically linked LGPL code (FMOPL, the
   Arduino core) needs one. Here everything is built from source by `tools/build.sh`, so the route is: change
   `vendor/tiny386/fmopl.c` (or the board package's core), rebuild, flash. Still to do: write that down as a
   release-specific procedure, check that it works from a clean machine with the pinned versions, and decide how to
   handle ESP-IDF's precompiled libraries in the board package (their sources are ESP-IDF v5.5.4 and
   esp32-arduino-lib-builder at the commit in the package's `versions.txt`).
4. **The BIOS images, pinned.** `vendor/bios.lock` records the tested images' SHA-256 and their source (SeaBIOS
   commit, tiny386's patch and config). `tools/fetch_vendor.sh` checks the images it gets. The images themselves
   come from tiny386's *continuous* release, which is rebuilt with every upstream push. The current one differs from
   the tested one only in its build stamp, but it is not a stable download. For a binary release:
   - either keep the exact images used, with their checksums, next to the release together with the SeaBIOS source
     archive, tiny386's `seabios/patch` and `seabios/config`, and the LGPL-3.0 and GPL-3.0 texts;
   - or build them with `BIOS_FROM_SOURCE=1 tools/fetch_vendor.sh` and publish the same set. The bytes then differ
     from the tested images, so test the firmware again. SeaBIOS also writes the build's date and host name into its
     version string, so build on a machine whose name may be published.
5. **`vgafont.c`**: tiny386's VGA text font has no notice in its file. Establish where it comes from and under which
   terms before distributing an image that contains it.
6. **The ESP32-C6's firmware** (the Tab5's Wi-Fi chip) is not part of this firmware and is not changed by it.

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
