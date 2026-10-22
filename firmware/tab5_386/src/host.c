// The platform layer the tiny386 core expects (pc.h "Platform HAL", and the hooks its ESP32 code paths refer to).
#include "tab5_config.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "pc.h"

extern const uint8_t bios_rom[], vgabios_rom[];
extern const size_t bios_rom_len, vgabios_rom_len;

uint32_t get_uticks() { return (uint32_t)esp_timer_get_time(); }

// Guest RAM, VGA memory and other large blocks: PSRAM, cache-line aligned. Never freed.
void* bigmalloc(size_t size) {
  void* p = heap_caps_aligned_calloc(128, 1, size, MALLOC_CAP_SPIRAM);
  if (!p) {
    fprintf(stderr, "bigmalloc: no PSRAM for %u bytes\n", (unsigned)size);
    abort();
  }
  return p;
}

// Device state goes into the hole in guest RAM between 640K and 768K (pc_new hands it over), as upstream does.
static char* pcram;
static long pcram_off, pcram_len;
void pcmalloc_init(void* ptr, long len) {
  pcram = ptr;
  pcram_off = 0;
  pcram_len = len;
}

void* pcmalloc(long size) {
  size = (size + 31) / 32 * 32;
  if (pcram_off + size > pcram_len) {
    fprintf(stderr, "pcmalloc: %ld bytes don't fit (%ld of %ld used)\n", size, pcram_off, pcram_len);
    abort();
  }
  void* p = pcram + pcram_off;
  pcram_off += size;
  return p;
}

// A ROM from a file (absolute path, e.g. on the SD card), or else the BIOS images built into the firmware.
// backward: the image ends at addr (the system BIOS ends at 1 MB).
int load_rom(void* phys_mem, const char* file, uword addr, int backward) {
  const uint8_t* rom = NULL;
  long len = 0;
  FILE* fp = file && file[0] == '/' ? fopen(file, "rb") : NULL;
  if (fp) {
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    rewind(fp);
    fread((char*)phys_mem + addr - (backward ? len : 0), 1, len, fp);
    fclose(fp);
    fprintf(stderr, "rom %s: %ld bytes\n", file, len);
    return len;
  }
  const char* base = file ? strrchr(file, '/') : NULL;
  base = base ? base + 1 : file;
  if (base && strcmp(base, "bios.bin") == 0) rom = bios_rom, len = bios_rom_len;
  else if (base && strcmp(base, "vgabios.bin") == 0) rom = vgabios_rom, len = vgabios_rom_len;
  if (!rom) {
    fprintf(stderr, "rom %s: not found\n", file ? file : "(none)");
    return 0;
  }
  memcpy((char*)phys_mem + addr - (backward ? len : 0), rom, len);
  fprintf(stderr, "rom %s: built in, %ld bytes\n", base, len);
  return len;
}

// ne2000.c's ESP32 build sends packets through this (no network yet), and ide.c's raw SD card disk
// ("/dev/mmcblk0") uses rawsd (not supported: disks are image files on the card).
void (*_Atomic esp32_send_packet)(uint8_t* buf, int size) = NULL;
void* rawsd = NULL;
