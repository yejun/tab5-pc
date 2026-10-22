// ESP-Hosted's SDIO host driver (the link to the WiFi chip, an ESP32-C6), compiled here in place of the copy in the
// Arduino core's library: the same version (2.12.8, vendor/esp-hosted.commit, fetched by tools/fetch_vendor.sh),
// with vendor/patches/esp-hosted. Its global symbols are the library's, so the linker doesn't take the library's.
//
// The changes (vendor/patches/esp-hosted/0001), mostly from newer ESP-Hosted (esp-hosted-mcu d7ebe00a, a8204f95):
// - a read whose buffer can't be allocated is dropped and tried again, where an assert reset the Tab5 (WiFi takes
//   ~160 KB of internal RAM, which ran out as the firmware grew); tried again even without a new interrupt, as the
//   C6 doesn't send another for data that is still waiting;
// - the read buffers, which grow to the longest burst, are in PSRAM (reads are whole blocks, which the SDMMC driver
//   takes there);
// - a packet length register reading all ones isn't taken as a length (as newer ESP-Hosted).
#include "sdio_drv.c"

