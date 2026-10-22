// tiny386 core: pc.c (see tab5_config.h)
#include "tab5_config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// pc.c reports every access to an I/O port it doesn't emulate with fprintf(stderr): here a 115200 baud UART that
// nobody reads, and a program that keeps using such a port (a joystick, say) would spend its time waiting for it.
// They are counted instead, the latest message kept (serial "cpu"). Other messages (at startup) go to stderr.
static volatile unsigned t386_port_count;
static char t386_port_last[48];

static int t386_fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n;
    if (f == stderr && (!strncmp(fmt, "out", 3) || !strncmp(fmt, "in", 2))) {
        n = vsnprintf(t386_port_last, sizeof(t386_port_last), fmt, ap);
        ++t386_port_count;
    } else {
        n = vfprintf(f, fmt, ap);
    }
    va_end(ap);
    return n;
}

void t386_port_report(char* buf, int len) {
    int n = snprintf(buf, len, "unemulated port accesses: %u, the last: %s", t386_port_count, t386_port_last);
    if (n > 0 && n < len && buf[n - 1] == '\n') buf[n - 1] = 0;
}

// Instructions per pc_step: tiny386's ESP32 default is 512, and the devices are polled (PIT, CMOS, keyboard, DMA)
// between steps, at a cost of a few hundred instructions' time. The PIT raises at most one interrupt per poll, so
// steps stay short while channel 0 runs fast (sampled sound through the PC speaker: up to ~16 kHz), and get up to
// t386_step_max long (serial "steps <n>") at the usual rates (18.2 Hz to ~1 kHz).
#include "i8254.h"
int t386_step_max = 2048;
static inline int t386_step_count(PITState* pit) {
    const int n = pit_get_initial_count(pit, 0) * 2;
    return n < 512 ? 512 : n > t386_step_max ? t386_step_max : n;
}
#define PC_STEP_COUNT t386_step_count(pc->pit)

// The VGA window's accesses (pc.c's iomem callbacks, reached from tiny386's own interpreter and string
// instructions) try t386_vga_ext.h's fast paths before vga.c's.
#include "vga.h"
int t386_vga_write_fast(uint32_t off, int size, uint32_t v);
int t386_vga_read_fast(uint32_t off, int size, uint32_t* out);
static inline uint8_t t386_vga_mem_read(VGAState* s, uint32_t off) {
    uint32_t v;
    return t386_vga_read_fast(off, 1, &v) ? v : vga_mem_read(s, off);
}
#define T386_VGA_WRITE(f, size) \
    static inline void t386_##f(VGAState* s, uint32_t off, uint32_t v) { \
        if (!t386_vga_write_fast(off, size, v)) f(s, off, v); \
    }
T386_VGA_WRITE(vga_mem_write, 1)
T386_VGA_WRITE(vga_mem_write16, 2)
T386_VGA_WRITE(vga_mem_write32, 4)
// ... and its registers' writes keep the translator's write mode (t386_vga_fw.h) up to date.
#include "t386_vga_fw.h"
static inline void t386_vga_ioport_write(VGAState* s, uint32_t addr, uint32_t val) {
    vga_ioport_write(s, addr, val);
    t386_vga_fw_update();
}
static inline void t386_vbe_write(VGAState* s, uint32_t offset, uint32_t val) {
    vbe_write(s, offset, val);
    t386_vga_fw_update();
}
// The vertical retrace (status register 3DA/3BA) as of the moment it is read: vga_step moves it on by the time. (Core 0
// did it every millisecond, but not while it draws the screen, 30-80 ms: a program could wait for it that long.)
int t386_vga_step(VGAState* s);   // (t386_vga_ext.h)
static inline uint32_t t386_vga_ioport_read(VGAState* s, uint32_t addr) {
    if (addr == 0x3da || addr == 0x3ba) t386_vga_step(s);
    return vga_ioport_read(s, addr);
}
#define vga_ioport_write t386_vga_ioport_write
#define vga_ioport_read t386_vga_ioport_read
#define vbe_write t386_vbe_write
#define vga_mem_read t386_vga_mem_read
#define vga_mem_write t386_vga_mem_write
#define vga_mem_write16 t386_vga_mem_write16
#define vga_mem_write32 t386_vga_mem_write32

#define fprintf t386_fprintf
#pragma GCC optimize("O3")
#include "pc.c"
