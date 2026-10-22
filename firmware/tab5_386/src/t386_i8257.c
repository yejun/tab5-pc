// tiny386 core: i8257.c (see tab5_config.h)
#include "tab5_config.h"
#pragma GCC optimize("O3")
#include <stdint.h>
void t386_ram_written(uint32_t addr, int len);   // ISA DMA into guest RAM: the translator checks (t386_jit.h)
#define T386_DMA_WRITE(addr, len) t386_ram_written(addr, len)
#include "i8257.c"
