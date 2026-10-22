// Set up tiny386's i386.c for the fast path (t386_fast.h) and the translator (t386_jit.h): included right before it.
// cpui386_step and cpui386_reset become tiny386's own (t386_fast.h defines the ones pc.c calls), and writes to guest
// RAM are reported to the translator (vendor/patches/0005).
#include <stdint.h>
#define cpui386_step tiny386_step
#define cpui386_reset tiny386_reset
extern uint8_t *t386_pagemap;
void t386_code_write(uint32_t addr, int len);
#define T386_RAM_WRITE(cpu, addr, len) do { \
		if (t386_pagemap) { \
			uint32_t _a = (addr); \
			int _n = (len); \
			if ((t386_pagemap[_a >> 12] | t386_pagemap[(_a + _n - 1) >> 12]) & 3) t386_code_write(_a, _n); \
		} \
	} while (0)
// tiny386's TLB emptied this many times with paging on (vendor/patches/0010): the fast path's follows. (Without paging
// its entries can't go stale, and paging switched on or off is a CR0 the fast path sees: DOSX switches protected mode
// on and off for every DOS call, each a MOV CR0 that empties tiny386's TLB.)
static unsigned t386_tlb_gen;
#define T386_TLB_CLEARED() (t386_tlb_gen += (cpu->cr0 >> 31) & 1)
