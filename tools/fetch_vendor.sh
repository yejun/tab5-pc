#!/usr/bin/env bash
# Fetch tiny386 at the pinned commit (vendor/tiny386.commit) into vendor/tiny386, and the BIOS images from its
# release into vendor/ (bios.bin, vgabios.bin; built into the firmware by tools/gen_bios.sh).
# Also ESP-Hosted's SDIO driver (vendor/esp-hosted.commit) into vendor/esp-hosted, see below.
set -euo pipefail
cd "$(dirname "$0")/../vendor"
COMMIT=$(cat tiny386.commit)
if [[ ! -d tiny386/.git ]]; then
  git clone https://github.com/hchunhui/tiny386 tiny386
fi
git -C tiny386 fetch --depth 1 origin "$COMMIT" 2>/dev/null || git -C tiny386 fetch origin
git -C tiny386 checkout -q "$COMMIT"
for p in patches/*.patch; do   # fixes of our own (see each patch)
  git -C tiny386 apply --check "../$p" 2>/dev/null && git -C tiny386 apply "../$p" && echo "applied $p"
done
make -C tiny386 prepare HOSTCC=${HOSTCC:-cc}
if [[ ! -f bios.bin || ! -f vgabios.bin ]]; then
  rm -rf tiny386-release && mkdir tiny386-release
  gh release download continuous -R hchunhui/tiny386 -p tiny386.tar.xz -D tiny386-release
  tar xJf tiny386-release/tiny386.tar.xz -C tiny386-release
  cp tiny386-release/tiny386/bios.bin tiny386-release/tiny386/vgabios.bin .
fi
echo "tiny386 $COMMIT ready"

# ESP-Hosted's SDIO host driver at 2.12.8, the version the Arduino core's library is built from, and the headers it
# uses, with vendor/patches/esp-hosted: the firmware compiles it in place of the library's copy
# (firmware/tab5_386/src/esp_hosted_sdio.c).
HCOMMIT=$(cat esp-hosted.commit)
rm -rf esp-hosted
for f in host/drivers/transport/sdio/sdio_drv.c host/drivers/transport/sdio/sdio_drv.h \
         host/drivers/transport/sdio/sdio_reg.h host/drivers/transport/transport_drv.h \
         host/drivers/transport/transport_util.h host/drivers/bt/hci_drv.h host/drivers/power_save/power_save_drv.h \
         host/drivers/serial/serial_drv.h host/drivers/serial/serial_ll_if.h host/utils/stats.h \
         host/esp_hosted_bt.h host/esp_hosted_event.h host/esp_hosted_os_abstraction.h \
         host/api/include/esp_hosted_api_types.h host/api/include/esp_hosted_power_save.h \
         host/api/include/esp_hosted_transport_config.h host/port/esp/freertos/include/port_esp_hosted_host_bt_config.h \
         host/port/esp/freertos/include/port_esp_hosted_host_config.h \
         host/port/esp/freertos/include/port_esp_hosted_host_os.h \
         host/port/esp/freertos/include/port_esp_hosted_host_sdio.h common/esp_hosted_header.h \
         common/esp_hosted_interface.h common/log/esp_hosted_log.h common/mempool/include/mempool.h \
         common/transport/esp_hosted_transport.h common/transport/esp_hosted_transport_init.h; do
  mkdir -p "esp-hosted/$(dirname "$f")"
  curl -sfL -o "esp-hosted/$f" "https://raw.githubusercontent.com/espressif/esp-hosted-mcu/$HCOMMIT/$f"
done
for p in patches/esp-hosted/*.patch; do
  (cd esp-hosted && git apply "../$p") && echo "applied $p"
done
echo "esp-hosted $HCOMMIT ready"
