// The emulator's code runs from PSRAM (tools/xip.ld says why and what): t386_xip_begin copies it there from flash
// and maps it at the address it was linked for. Nothing of the t386_*.c files may be called before.
#pragma once
#define T386_XIP_MAX (15 * 65536)   // room in flash for it (tools/xip_patch.py checks)
#ifdef __cplusplus
extern "C" {
#endif
extern const unsigned char t386_xip_store[T386_XIP_MAX];
int t386_xip_begin(void);   // 0, or what failed
long t386_xip_check(unsigned* size);   // the code in PSRAM against flash: -1 the same, else the first byte that differs
#ifdef __cplusplus
}
#endif
