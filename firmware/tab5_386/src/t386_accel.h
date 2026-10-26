// A 2D engine for the linear frame buffer, for Linux's graphics console (a patch to the guest kernel's vesafb, for
// its fillrect, copyarea and imageblit): drawing a line of text in the guest took ~40 ms of emulated instructions,
// here it is a few host loops. Included after pc.c (t386_pc.c); t386_accel_hooks() puts its port in front of
// pc.c's. Without the patch in the guest, the port is never used.
//
// Port T386_ACCEL_PORT: reading it (32 bits) gives T386_ACCEL_MAGIC (no engine: all ones); writing it the physical
// address of a command block in guest RAM, which is done before the OUT returns, and its result word set (0: done).
// Offsets are bytes in video memory (the linear frame buffer's start is 0); widths are pixels of bpp bytes.
#pragma once
#include <stddef.h>
#include <string.h>
#include "t386_vga_fw.h"

#define T386_ACCEL_PORT 0xf1e0
#define T386_ACCEL_MAGIC 0x7386acceu
enum { T386_ACCEL_FILL = 1, T386_ACCEL_COPY = 2, T386_ACCEL_MONO = 3 };
struct t386_accel_cmd {
    uint32_t op;
    uint32_t dst, pitch, width, height, bpp;   // the rectangle written (bpp: 1, 2 or 4)
    uint32_t fg, bg;                           // FILL: fg the colour; MONO: the colours for 1 and 0 bits
    uint32_t src, src_pitch;                   // COPY: the rectangle's offset (pitch: dst's); MONO: the bitmap's
                                               // physical address and bytes a row (the first pixel in bit 7)
    uint32_t rop;                              // FILL: 0 copy, 1 xor
    uint32_t result;
};

void t386_ram_written(uint32_t addr, int len);   // (guest RAM written from outside the CPU: the translator checks)
static u32 (*t386_accel_prev_read32)(void*, int);
static void (*t386_accel_prev_write32)(void*, int, u32);

// a rectangle of video memory whole within it
static bool t386_accel_fits(PC* pc, uint32_t off, uint32_t pitch, uint32_t row_bytes, uint32_t height) {
    const uint64_t end = (uint64_t)off + (uint64_t)pitch * (height - 1) + row_bytes;
    return height && row_bytes && row_bytes <= pitch && end <= (uint64_t)pc->vga_mem_size;
}

static void t386_accel_put(uint8_t* p, uint32_t bpp, uint32_t v) {
    if (bpp == 1) *p = v;
    else if (bpp == 2) memcpy(p, &v, 2);
    else memcpy(p, &v, 4);
}

static uint32_t t386_accel_run(PC* pc, struct t386_accel_cmd* c) {
    const uint32_t bpp = c->bpp, row = c->width * bpp;
    if ((bpp != 1 && bpp != 2 && bpp != 4) || c->width > 4096 || c->height > 4096 ||
        !t386_accel_fits(pc, c->dst, c->pitch, row, c->height))
        return 1;
    uint8_t* vram = pc->vga_mem;
    uint8_t* d = vram + c->dst;
    switch (c->op) {
    case T386_ACCEL_FILL:
        for (uint32_t y = 0; y < c->height; y++, d += c->pitch) {
            if (c->rop) {
                for (uint32_t x = 0; x < row; x += bpp) {
                    uint32_t v = 0;
                    memcpy(&v, d + x, bpp);
                    t386_accel_put(d + x, bpp, v ^ c->fg);
                }
            } else if (bpp == 1) {
                memset(d, c->fg, row);
            } else {
                for (uint32_t x = 0; x < row; x += bpp) t386_accel_put(d + x, bpp, c->fg);
            }
        }
        break;
    case T386_ACCEL_COPY: {
        if (!t386_accel_fits(pc, c->src, c->pitch, row, c->height)) return 1;
        const uint8_t* s = vram + c->src;
        if (c->dst > c->src) {   // (overlapping: the last row first)
            d += c->pitch * (c->height - 1);
            s += c->pitch * (c->height - 1);
            for (uint32_t y = 0; y < c->height; y++, d -= c->pitch, s -= c->pitch) memmove(d, s, row);
        } else {
            for (uint32_t y = 0; y < c->height; y++, d += c->pitch, s += c->pitch) memmove(d, s, row);
        }
        break;
    }
    case T386_ACCEL_MONO: {
        const uint64_t bits_end = (uint64_t)c->src + (uint64_t)c->src_pitch * c->height;
        if (c->src_pitch < (c->width + 7) / 8 || bits_end > (uint64_t)pc->phys_mem_size) return 1;
        const uint8_t* s = (const uint8_t*)pc->phys_mem + c->src;
        for (uint32_t y = 0; y < c->height; y++, d += c->pitch, s += c->src_pitch) {
            uint8_t* p = d;
            for (uint32_t x = 0; x < c->width; x++, p += bpp) {
                const uint32_t v = (s[x >> 3] >> (7 - (x & 7))) & 1 ? c->fg : c->bg;
                t386_accel_put(p, bpp, v);
            }
        }
        break;
    }
    default:
        return 1;
    }
    t386_vram_mark(c->dst, c->pitch * (c->height - 1) + row);
    return 0;
}

static u32 t386_accel_read32(void* o, int addr) {
    if (addr == T386_ACCEL_PORT) return T386_ACCEL_MAGIC;
    return t386_accel_prev_read32(o, addr);
}

static void t386_accel_write32(void* o, int addr, u32 val) {
    if (addr != T386_ACCEL_PORT) {
        t386_accel_prev_write32(o, addr, val);
        return;
    }
    PC* pc = o;
    struct t386_accel_cmd c;
    if ((uint64_t)val + sizeof(c) > (uint64_t)pc->phys_mem_size) return;
    memcpy(&c, (uint8_t*)pc->phys_mem + val, sizeof(c));
    const uint32_t result = t386_accel_run(pc, &c);
    memcpy((uint8_t*)pc->phys_mem + val + offsetof(struct t386_accel_cmd, result), &result, 4);
    t386_ram_written(val + offsetof(struct t386_accel_cmd, result), 4);
}

static void t386_accel_hooks(CPU_CB* cb) {
    t386_accel_prev_read32 = cb->io_read32;
    t386_accel_prev_write32 = cb->io_write32;
    cb->io_read32 = t386_accel_read32;
    cb->io_write32 = t386_accel_write32;
}
