#include "xip.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "hal/mmu_hal.h"
#include "hal/mmu_ll.h"
#include "soc/soc.h"
#include "soc/cache_reg.h"
#include "esp32p4/rom/cache.h"
#include "esp_private/esp_cache_private.h"

extern char _t386_xip_start __attribute__((weak)), _t386_xip_end __attribute__((weak));   // (XIP=0 tools/build.sh: none)

static void xip_cache_op(uint32_t map, uint32_t op, uintptr_t a, uint32_t n) {
    esp_cache_sync_ops_enter_critical_section();
    REG_WRITE(CACHE_SYNC_MAP_REG, map);
    REG_WRITE(CACHE_SYNC_ADDR_REG, a);
    REG_WRITE(CACHE_SYNC_SIZE_REG, n);
    for (int i = 0; i < 2; i++) {
        REG_WRITE(CACHE_SYNC_CTRL_REG, op);
        while (!REG_GET_BIT(CACHE_SYNC_CTRL_REG, CACHE_SYNC_DONE)) {}
    }
    esp_cache_sync_ops_exit_critical_section();
}

int t386_xip_begin(void) {
    const uintptr_t at = (uintptr_t)&_t386_xip_start;
    const size_t n = &_t386_xip_end - &_t386_xip_start, len = (n + 0xffff) & ~(size_t)0xffff;
    if (!n) return 0;
    if (n > T386_XIP_MAX || (at & 0xffff)) return 1;
    // whole 64 KB pages of PSRAM, from the heap (never freed, and not touched through this address again) ...
    uint8_t* p = (uint8_t*)heap_caps_aligned_alloc(0x10000, len, MALLOC_CAP_SPIRAM);
    if (!p) return 2;
    memcpy(p, t386_xip_store, n);
    memset(p + n, 0, len - n);
    asm volatile("fence rw, rw" ::: "memory");
    xip_cache_op(CACHE_MAP_L1_DCACHE, CACHE_WRITEBACK_ENA, (uintptr_t)p, len);
    xip_cache_op(CACHE_MAP_L2_CACHE, CACHE_WRITEBACK_ENA, (uintptr_t)p, len);
    xip_cache_op(CACHE_MAP_L1_DCACHE, CACHE_INVALIDATE_ENA, (uintptr_t)p, len);
    xip_cache_op(CACHE_MAP_L2_CACHE, CACHE_INVALIDATE_ENA, (uintptr_t)p, len);
    // ... and the same pages once more at the address the code was linked for
    esp_paddr_t paddr;
    mmu_target_t target;
    if (esp_mmu_vaddr_to_paddr(p, &paddr, &target) != ESP_OK || target != MMU_TARGET_PSRAM0) return 3;
    uint32_t mapped = 0;
    mmu_hal_map_region(MMU_LL_PSRAM_MMU_ID, MMU_TARGET_PSRAM0, at, paddr, len, &mapped);
    if (mapped != len) return 4;
    xip_cache_op(CACHE_MAP_L1_ICACHE_MASK | CACHE_MAP_L1_DCACHE, CACHE_INVALIDATE_ENA, at, len);
    xip_cache_op(CACHE_MAP_L2_CACHE, CACHE_INVALIDATE_ENA, at, len);
    asm volatile("fence.i" ::: "memory");
    return memcmp((const void*)at, t386_xip_store, n) ? 5 : 0;
}

long t386_xip_check(unsigned* size) {
    const unsigned char* at = (const unsigned char*)&_t386_xip_start;
    const size_t n = &_t386_xip_end - &_t386_xip_start;
    if (size) *size = n;
    for (size_t i = 0; i < n; i++)
        if (at[i] != t386_xip_store[i]) return (long)i;
    return -1;
}
