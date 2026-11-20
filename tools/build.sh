#!/usr/bin/env bash
# Build (and optionally flash) the Tab5 PC firmware:  tools/build.sh [upload [PORT]]
#
#   CHIP=postv3 tools/build.sh upload    for ESP32-P4 chips of revision v3.0 and later
#
# The default, CHIP=prev3, is for earlier chips: esptool prints "revision v1.x" for them. A build for the wrong
# revision does not boot.
set -euo pipefail
if [[ $# -gt 2 || ( $# -gt 0 && "$1" != "upload" ) ]]; then
  echo "Usage: [CHIP=prev3|postv3] $0 [upload [PORT]]" >&2
  exit 2
fi
cd "$(dirname "$0")/.."
ROOT=$PWD
CHIP=${CHIP:-prev3}
[[ "$CHIP" == prev3 || "$CHIP" == postv3 ]] || { echo "CHIP must be prev3 or postv3" >&2; exit 2; }
FQBN="m5stack:esp32:m5stack_tab5:ChipVariant=$CHIP,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default"
PORT=${2:-/dev/ttyACM0}
SKETCH="$ROOT/firmware/tab5_386"
BUILD="$ROOT/build/$CHIP"
# the tiny386 core (compiled through the wrappers in firmware/tab5_386/src), and LVGL finds lv_conf.h in the sketch
FLAGS="-I$ROOT/vendor/tiny386 -DLV_CONF_INCLUDE_SIMPLE -I$SKETCH ${DEFS:-}"   # DEFS: e.g. -DT386_FPU_STATS
# source file names in the image (assert messages) relative to their tree, not this computer's directories
ADATA=$(arduino-cli config get directories.data 2>/dev/null || echo "$HOME/.arduino15")
AUSER=$(arduino-cli config get directories.user 2>/dev/null || echo "$HOME/Arduino")
FLAGS="$FLAGS -ffile-prefix-map=$ROOT=tab5-pc -ffile-prefix-map=$AUSER=Arduino -ffile-prefix-map=$ADATA=arduino15"
# ESP-Hosted's SDIO driver, patched (vendor/esp-hosted, from tools/fetch_vendor.sh; src/esp_hosted_sdio.c)
[[ -f "$ROOT/vendor/esp-hosted/host/drivers/transport/sdio/sdio_drv.c" ]] || { echo "run tools/fetch_vendor.sh first" >&2; exit 1; }
H=$ROOT/vendor/esp-hosted
for d in host/drivers/transport/sdio host/drivers/transport host/drivers/bt host/drivers/power_save host/drivers/serial \
         host/utils host host/api/include host/port/esp/freertos/include common common/log common/mempool/include \
         common/transport; do
  FLAGS="$FLAGS -I$H/$d"
done
[[ -f "$ROOT/vendor/tiny386/fmopl.inc" ]] || make -C "$ROOT/vendor/tiny386" prepare HOSTCC=${HOSTCC:-cc}
# the BIOS images are built into the firmware, but not kept in the repository (SeaBIOS, LGPL-3): from vendor/*.bin
B="$SKETCH/src/bios_rom.c"
[[ -f "$B" && "$B" -nt "$ROOT/vendor/bios.bin" && "$B" -nt "$ROOT/vendor/vgabios.bin" ]] || "$ROOT/tools/gen_bios.sh"
# The emulator's code is linked to run from PSRAM (tools/xip.ld), its bytes moved into the flash image after the link
# (tools/xip_patch.py). The link finds "sections.ld" in the directory it runs in before the core's: ours, made from
# the core's. XIP=0 tools/build.sh builds without, everything in flash as up to 1.7.0 (for comparisons).
XIPP=()
if [[ "${XIP:-1}" != 0 ]]; then
  XIPP=(--build-property "recipe.hooks.linking.prelink.1.pattern=python3 $ROOT/tools/xip_patch.py ld {compiler.sdk.path}/ld/sections.ld {build.path}.ld/sections.ld"
        --build-property "recipe.hooks.linking.postlink.1.pattern=python3 $ROOT/tools/xip_patch.py {build.path}/{build.project_name}.elf")
  mkdir -p "$BUILD.ld"
  cd "$BUILD.ld"
else
  rm -rf "$BUILD.ld"
fi
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.c.extra_flags=$FLAGS" --build-property "compiler.cpp.extra_flags=$FLAGS" "${XIPP[@]}" "$SKETCH"
cd "$ROOT"
[[ -f "$BUILD/tab5_386.ino.full.elf" && "${XIP:-1}" != 0 ]] || cp "$BUILD/tab5_386.ino.elf" "$BUILD/tab5_386.ino.full.elf"
# Translated code keeps a host address in tp (t386_jit.h, R_GRP): nothing in the firmware may read it. Only an
# interrupt's save and restore and the translator's own routine touch it now; thread-local variables would read it.
# (tools/tp_check.py)
OBJDUMP=$(ls "$ADATA"/packages/esp32/tools/esp-rv32/*/bin/riscv32-esp-elf-objdump 2>/dev/null | tail -1 || true)
if [[ -n "$OBJDUMP" ]]; then
  python3 "$ROOT/tools/tp_check.py" "$OBJDUMP" "$BUILD/tab5_386.ino.full.elf"
else
  echo "no objdump found: the firmware's uses of tp were not checked (t386_jit.h, R_GRP)" >&2
fi
if [[ "${1:-}" == "upload" ]]; then
  arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$BUILD"
fi
