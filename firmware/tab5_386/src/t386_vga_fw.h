// The VGA's current write mode, for translated code's writes to video memory (t386_jit.h, jit_make_vga_writes): set
// in t386_vga_ext.h by t386_vga_fw_update(), which runs after every write to the VGA's registers (t386_pc.c)
// and when t386_vga changes.
#pragma once
#include <stdint.h>

// Which pieces of video memory (1 << t386_vram_shift bytes each, at most 1024 pieces) were written since the native
// renderer last looked: for VBE modes, so it reads only the lines in those (t386_vga_ext.h). Marked by the linear frame
// buffer's writes (t386_pc.c: pc.c's iomem callbacks, which the fast path, translated code and tiny386's own
// interpreter all end in) and the 64 KB window's (t386_vga_write_fast, and vga.c's when it doesn't take them).
extern uint8_t t386_vram_dirty[1024];
extern int t386_vram_shift;
// (one more each time the renderer takes and clears the marks: t386_fast.h's armed pages)
extern uint32_t t386_vram_epoch;

// The linear frame buffer (BAR 0 of the PCI VGA) while it is mapped: its physical address and size, and its bytes
// (tiny386's video memory); size 0: unmapped. The fast path maps its pages straight to them (t386_fast.h, ftlb_miss),
// marking the pieces writes change. Set by pc.c's BAR callback (t386_pc.c).
extern uint32_t t386_lfb_base, t386_lfb_size;
extern uint8_t* t386_lfb_ram;
static inline void t386_vram_mark(uint32_t off, uint32_t n) {
    const uint32_t last = (off + n - 1) >> t386_vram_shift;
    for (uint32_t i = off >> t386_vram_shift; i <= last && i < 1024; i++) t386_vram_dirty[i] = 1;
}

enum {
    T386_FW_NONE,     // the usual way (t386_jit_wr1/2/4)
    T386_FW_PLANAR,   // planar (EGA modes, mode X), write mode 0 as a plain store: a byte to the planes in wm
    T386_FW_LATCH,    // planar, write mode 1: the latches to the planes in wm
    T386_FW_CHAIN4,   // chain 4 (mode 13h): a byte to video memory if its plane (address & 3) is in pm
    T386_FW_PLANE1,   // T386_FW_PLANAR with one plane written (mode Y, column by column): a byte to it
    T386_FW_LINEAR,   // T386_FW_CHAIN4 with all four planes written (mode 13h as a rule): a byte to video memory
    T386_FW_WM0,      // planar, write mode 0 with rotation, set/reset, a logical operation or a bit mask (as vga.c)
    T386_FW_WM2,      // planar, write mode 2: the byte's low 4 bits a colour, in the bit mask's dots
    T386_FW_WM3,      // planar, write mode 3: set/reset's colour in the dots of the rotated byte & the bit mask
};

struct T386VgaFw {
    uint32_t kind;    // T386_FW_*
    uint32_t wm;      // the planes written: a byte of ones for each
    uint8_t* ram;     // video memory (the window at A0000, memory map mode 1, no bank offset)
    uint32_t* latch;  // the VGA's latches
    uint32_t pm;      // the plane mask (chain 4)
    uint8_t* plane1;  // T386_FW_PLANE1: video memory's byte of that plane for offset 0 (offset n: plane1 + 4n)
    // T386_FW_WM*: the graphics controller's registers as the write needs them
    uint32_t rot, rot8;   // the rotation (0-7), and 8 - it
    uint32_t op;          // the logical operation with the latches: 0 none, 1 AND, 2 OR, 3 XOR
    uint32_t bm8, bm32;   // the bit mask, and it in each plane's byte
    uint32_t nsetm;       // write mode 0: the planes set/reset doesn't take, a byte of ones each
    uint32_t srv;         // set/reset's colour as the planes' bytes (write mode 0: of the planes it takes)
    const uint32_t* m16;  // write mode 2: a colour (0-15) as the planes' bytes
};
extern struct T386VgaFw t386_vga_fw;
void t386_vga_fw_update(void);
