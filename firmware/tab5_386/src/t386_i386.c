// tiny386 core: i386.c (see tab5_config.h)
#include "tab5_config.h"
#pragma GCC optimize("O3")

// the translator to RISC-V (t386_jit.h): its code cache in PSRAM, which this chip revision can execute
#define T386_JIT
#define T386_JIT_CACHE_SIZE (3 << 20)
// the fast path's pre-decoded blocks (t386_fast_pd.h): only while the translator is off, in its code cache (PSRAM
// has little room left: WiFi's packets, uploads)
#define T386_PD_ARENA 0
#define T386_PD_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_cpu.h"
static void* t386_jit_alloc(size_t n) { return heap_caps_aligned_calloc(128, 1, n, MALLOC_CAP_SPIRAM); }
uint32_t t386_sync_calls, t386_sync_cycles;   // (serial "S")
// New code, written through the L1 data cache, for the instruction fetch: the L1 data cache's lines written back to
// the L2 cache (which both share), then the L1 instruction caches' lines dropped. (esp_cache_msync on PSRAM also wrote
// L2 back to PSRAM and dropped the lines from L1 data and L2: ~4000 cycles a call, and the new code fetched from PSRAM
// again.) Under esp_cache_msync's own lock: core 0's cache operations use the same controller. The stores are done
// first (fence: none left in the store buffer when the cache controller writes the lines back), and the core's own
// fetch forgets what it fetched before (fence.i).
// Each operation is done twice, as ESP-IDF's own version of the ROM's write-back routine does
// (esp_rom_cache_writeback_esp32p4*.c); this build links the ROM's. With one pass, the instruction fetch now and then
// ran a block as first emitted, before its jumps were patched: a branch to its own stub still pointing at itself
// was caught with a debugger, while memory held it patched.
#include "soc/cache_reg.h"
#include "esp32p4/rom/cache.h"
#include "esp_private/esp_cache_private.h"
static void t386_cache_op(uint32_t map, uint32_t op, uintptr_t a, uint32_t n) {
  REG_WRITE(CACHE_SYNC_MAP_REG, map);
  REG_WRITE(CACHE_SYNC_ADDR_REG, a);
  REG_WRITE(CACHE_SYNC_SIZE_REG, n);
  for (int i = 0; i < 2; i++) {
    REG_WRITE(CACHE_SYNC_CTRL_REG, op);
    while (!REG_GET_BIT(CACHE_SYNC_CTRL_REG, CACHE_SYNC_DONE)) {}
  }
}
static void t386_jit_sync(void* p, size_t n) {
  const uint32_t c0 = esp_cpu_get_cycle_count();
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)127, e = ((uintptr_t)p + n + 127) & ~(uintptr_t)127;
  asm volatile("fence rw, rw" ::: "memory");
  esp_cache_sync_ops_enter_critical_section();
  t386_cache_op(CACHE_MAP_L1_DCACHE, CACHE_WRITEBACK_ENA, a, e - a);
  t386_cache_op(CACHE_MAP_L1_ICACHE_MASK, CACHE_INVALIDATE_ENA, a, e - a);
  esp_cache_sync_ops_exit_critical_section();
  asm volatile("fence.i" ::: "memory");
  t386_sync_cycles += esp_cpu_get_cycle_count() - c0;
  t386_sync_calls++;
}

#include "t386_cpu_wrap.h"
#include "i386.c"
#undef cpui386_step
#undef cpui386_reset
#include "t386_fast.h"

// Tab5 additions with access to the CPU's internals.
#include "esp_memory_utils.h"
void t386_cpu_report(CPUI386* cpu, char* buf, int len) {
  snprintf(buf, len, "cpu state %p %s (%u bytes) | tlb %p %s", cpu, esp_ptr_external_ram(cpu) ? "PSRAM" : "internal",
           (unsigned)sizeof(*cpu), cpu->tlb.tab, esp_ptr_external_ram(cpu->tlb.tab) ? "PSRAM" : "internal");
}

// The hardware interrupts taken and the INT instructions run since the last call, the most first (serial "cpu")
extern unsigned t386_int_hw[256], t386_int_vec[256];
static int t386_top(char* buf, int len, unsigned* count) {
    int n = 0;
    for (int k = 0; k < 8 && n < len - 24; k++) {
        int best = -1;
        for (int v = 0; v < 256; v++)
            if (count[v] && (best < 0 || count[v] > count[best])) best = v;
        if (best < 0) break;
        n += snprintf(buf + n, len - n, " %02x: %u", best, count[best]);
        count[best] = 0;
    }
    memset(count, 0, 256 * sizeof(unsigned));
    return n;
}
void t386_irq_report(char* buf, int len) {
    int n = snprintf(buf, len, "since the last report: irqs by vector");
    n += t386_top(buf + n, len - n, t386_int_hw);
    n += snprintf(buf + n, len - n, " | INT instructions by vector");
    t386_top(buf + n, len - n, t386_int_vec);
}

// The guest CPU's registers and the code at CS:EIP (serial "cpu"): where a program is, e.g. one that hangs.
void t386_cpu_regs(CPUI386* cpu, char* buf, int len) {
    const uword cs_base = cpu->seg[SEG_CS].base, lin = cs_base + cpu->ip;
    int n = snprintf(buf, len,
                     "cs:eip %04x:%08x (lin %08x) ss:esp %04x:%08x ds %04x es %04x | eax %08x ebx %08x ecx %08x edx %08x "
                     "esi %08x edi %08x ebp %08x | flags %08x (IF %d) cr0 %08x %s%s | code",
                     (unsigned)cpu->seg[SEG_CS].sel, (unsigned)cpu->ip, (unsigned)lin, (unsigned)cpu->seg[SEG_SS].sel,
                     (unsigned)REGi(4), (unsigned)cpu->seg[SEG_DS].sel, (unsigned)cpu->seg[SEG_ES].sel,
                     (unsigned)REGi(0), (unsigned)REGi(3), (unsigned)REGi(1), (unsigned)REGi(2), (unsigned)REGi(6),
                     (unsigned)REGi(7), (unsigned)REGi(5), (unsigned)cpu->flags, !!(cpu->flags & IF), (unsigned)cpu->cr0,
                     cpu->halt ? "halted " : "", cpu->intr ? "irq pending" : "");
    for (int i = 0; i < 16 && n < len - 4; ++i)   // real mode / unpaged: linear = physical
        n += snprintf(buf + n, len - n, " %02x", lin + i < (uword)cpu->phys_mem_size ? cpu->phys_mem[lin + i] : 0xff);
}
