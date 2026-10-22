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
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.c.extra_flags=$FLAGS" --build-property "compiler.cpp.extra_flags=$FLAGS" "$SKETCH"
if [[ "${1:-}" == "upload" ]]; then
  arduino-cli upload --fqbn "$FQBN" --port "$PORT" --input-dir "$BUILD"
fi
