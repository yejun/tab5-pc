// The VGA's current write mode, for translated code's writes to video memory (t386_jit.h, jit_make_vga_writes): set
// in t386_vga_ext.h by t386_vga_fw_update(), which runs after every write to the VGA's registers (t386_pc.c)
// and when t386_vga changes.
#pragma once
#include <stdint.h>

enum {
    T386_FW_NONE,     // the usual way (t386_jit_wr1/2/4)
    T386_FW_PLANAR,   // planar (EGA modes, mode X), write mode 0 as a plain store: a byte to the planes in wm
    T386_FW_LATCH,    // planar, write mode 1: the latches to the planes in wm
    T386_FW_CHAIN4,   // chain 4 (mode 13h): a byte to video memory if its plane (address & 3) is in pm
    T386_FW_PLANE1,   // T386_FW_PLANAR with one plane written (mode Y, column by column): a byte to it
    T386_FW_LINEAR,   // T386_FW_CHAIN4 with all four planes written (mode 13h as a rule): a byte to video memory
};

struct T386VgaFw {
    uint32_t kind;    // T386_FW_*
    uint32_t wm;      // the planes written: a byte of ones for each
    uint8_t* ram;     // video memory (the window at A0000, memory map mode 1, no bank offset)
    uint32_t* latch;  // the VGA's latches
    uint32_t pm;      // the plane mask (chain 4)
    uint8_t* plane1;  // T386_FW_PLANE1: video memory's byte of that plane for offset 0 (offset n: plane1 + 4n)
};
extern struct T386VgaFw t386_vga_fw;
void t386_vga_fw_update(void);
