// Tab5 additions to tiny386's vga.c, with access to the VGA's internals: included after it (t386_vga.c).
#include "t386_vga_native.h"

// A fingerprint of everything that decides the picture (registers, palette, the video memory on show, and the text cursor's blink phase), so the screen is redrawn
// only when it changed.
uint32_t t386_mix(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = p;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 0x9E3779B1u;
        h ^= h >> 15;
    }
    for (; i < n; ++i) h = (h ^ b[i]) * 0x01000193u;
    return h;
}

uint32_t t386_vga_frame_hash(VGAState* s) {
    uint32_t h = 0x811C9DC5u;
    h = t386_mix(h, s->sr, sizeof(s->sr));
    h = t386_mix(h, s->gr, sizeof(s->gr));
    h = t386_mix(h, &s->ar_index, 1);
    h = t386_mix(h, s->ar, sizeof(s->ar));
    h = t386_mix(h, s->cr, 0x19);   // the CRT registers that exist (cursor, start address, timing)
    h = t386_mix(h, s->palette, sizeof(s->palette));
    h = t386_mix(h, &s->dac_8bit, 1);
    h = t386_mix(h, s->vbe_regs, sizeof(s->vbe_regs));
    h = t386_mix(h, &s->vbe_start_addr, sizeof(s->vbe_start_addr));
    h = t386_mix(h, &s->vbe_line_offset, sizeof(s->vbe_line_offset));
    const bool text = !(s->gr[0x06] & 1);
    // The text cursor blinks at a real VGA's rate (on 16 frames, off 16), not tiny386's twice as fast: half the
    // redraws at a DOS prompt. Its phase is set here and tiny386's own blink timer kept from firing.
    const uint32_t now = get_uticks();
    s->cursor_visible_phase = (now / 266667) & 1;
    s->cursor_blink_time = now + 0x40000000;
    if ((s->ar_index & 0x20) && text && !(s->cr[0x0a] & 0x20)) {   // text mode, cursor shown
        uint32_t phase = s->cursor_visible_phase;
        h = t386_mix(h, &phase, sizeof(phase));
    }
    // the video memory on show, the way vga_text_refresh / vga_graphic_refresh walk it
    size_t start, len;
    if (vbe_enabled(s)) {
        start = 4 * (size_t)s->vbe_start_addr;
        len = (size_t)s->vbe_line_offset * s->vbe_regs[VBE_DISPI_INDEX_YRES];
    } else {
        start = 4 * (size_t)(s->cr[0x0d] | (s->cr[0x0c] << 8));
        size_t lines = (s->cr[0x12] | ((s->cr[0x07] & 0x02) << 7) | ((s->cr[0x07] & 0x40) << 3)) + 1;
        if (text) lines = lines / ((s->cr[0x09] & 0x1f) + 1) + 1;   // character rows
        len = ((size_t)s->cr[0x13] << 3) * lines;
        if ((s->cr[0x17] & 3) != 3 || len < 0x10000) len = 0x10000;    // CGA interleaving, or small: a whole 64K
        // (not the fonts: a program that changes only the font, not the text, isn't noticed until it does)
    }
    if (start > (size_t)s->vga_ram_size) start = 0;
    if (start + len > (size_t)s->vga_ram_size) len = s->vga_ram_size - start;
    return t386_mix(h, s->vga_ram + start, len);
}

// Copy the video memory the next vga_refresh will read into `shadow` (as large as the video memory) and point the
// refresh at it. A refresh takes up to ~80 ms on core 0, while the program goes on running on core 1: a game that
// flips pages would already be clearing and redrawing the page being converted (black patches, from the top left).
// The copy takes a moment. NULL: refresh from the video memory itself again.
void t386_vga_snapshot(VGAState* s, uint8_t* shadow) {
    if (shadow) {
        size_t start = 0, len = s->vga_ram_size < 0x40000 ? s->vga_ram_size : 0x40000;   // planes, text, fonts
        if (vbe_enabled(s)) {
            start = 4 * (size_t)s->vbe_start_addr;
            len = (size_t)s->vbe_line_offset * s->vbe_regs[VBE_DISPI_INDEX_YRES];
            if (start > (size_t)s->vga_ram_size) start = 0;
            if (start + len > (size_t)s->vga_ram_size) len = s->vga_ram_size - start;
        }
        memcpy(shadow + start, s->vga_ram + start, len);
    }
    s->vga_ram_view = shadow;
}

// The VGA's graphics modes with 16 colours (EGA 0Dh, 0Eh, 10h, VGA 12h, planar) and 256 (13h, Mode X; not VBE),
// drawn at their own resolution (320 x 200 ... 720 x 480 pixels) as RGB565 into dst, for the PPA to scale onto the
// panel. tiny386's vga_refresh draws them dot by dot into 720 x 480 (~35-65 ms a frame); here each line's video
// memory is fingerprinted and only the lines that changed are converted (8 planar pixels at a time through tables),
// and dirty[y] says which. Also what tiny386 leaves out: the pixel panning (smooth sideways scrolling) and
// the line compare (split screen), and the 256 KB wrap of the video memory. False for other modes (text, CGA, VBE).
#define NAT_MAX_H 480
static uint32_t nat_row_hash[NAT_MAX_H];
static uint32_t nat_params;
static uint32_t nat_expand[256];   // bit 7 - i of the byte -> bit 0 of nibble i (pixel i)
static uint8_t nat_line[768];   // a line's bytes where it wraps around the end of the 256 KB

static const uint8_t* nat_src(VGAState* s, uint32_t addr, uint32_t n) {
    addr &= 0x3ffff;
    if (addr + n <= 0x40000) return s->vga_ram + addr;
    const uint32_t first = 0x40000 - addr;
    memcpy(nat_line, s->vga_ram + addr, first);
    memcpy(nat_line + first, s->vga_ram, n - first);
    return nat_line;
}

bool t386_vga_render_native(VGAState* s, uint16_t* dst, uint8_t* dirty, int max_w, int max_h, bool force,
                            T386Native* out) {
    if (!(s->gr[0x06] & 1) || vbe_enabled(s) || s->vga_ram_size < 0x40000) return false;   // text, VBE
    const int shift_control = (s->gr[0x05] >> 5) & 3;
    if (shift_control == 1) return false;                      // CGA 4 colours
    if ((s->cr[0x17] & 3) != 3) return false;                  // CGA / Hercules line interleaving
    const bool c256 = shift_control >= 2;
    const uint8_t plane_mask = s->ar[0x12] & 0x0f;
    if (!c256 && plane_mask == 1 && s->comp_ntsc) return false;   // tiny386's composite colours
    const int pw = c256 ? (s->cr[0x01] + 1) * 4 : (s->cr[0x01] + 1) * 8;
    const int dx = c256 || (s->sr[0x01] & 8) ? 2 : 1;          // dots per pixel
    const int out_h = (s->cr[0x12] | ((s->cr[0x07] & 0x02) << 7) | ((s->cr[0x07] & 0x40) << 3)) + 1;
    const int ly = (s->cr[0x09] >> 7) ? 2 : (s->cr[0x09] & 0x1f) + 1;   // scan lines per pixel line
    const int h = out_h / ly;
    if (pw <= 0 || h <= 0 || pw > max_w || h > max_h || h > NAT_MAX_H || pw > 720 || (pw & 7)) return false;
    if (pw * dx > 720 || out_h > 480) return false;            // (the panel's 1080 x 720 at 1.5)

    uint32_t pal[256];
    if (c256) update_palette256(s, pal); else update_palette16(s, pal);
    // what decides every line's colours and the layout: when it changes, all lines are drawn again
    uint32_t params = t386_mix(0x811C9DC5u, pal, (c256 ? 256 : 16) * sizeof(uint32_t));
    const uint32_t geo[6] = { (uint32_t)pw, (uint32_t)h, (uint32_t)dx, (uint32_t)ly, plane_mask, c256 };
    params = t386_mix(params, geo, sizeof(geo));
    if (force || params != nat_params) {
        for (int y = 0; y < h; y++) nat_row_hash[y] = ~0u;
        nat_params = params;
    }

    const uint32_t line_offset = (uint32_t)s->cr[0x13] << 3;
    const uint32_t start = 4 * (uint32_t)(s->cr[0x0d] | (s->cr[0x0c] << 8));
    const uint32_t line_compare = s->cr[0x18] | ((s->cr[0x07] & 0x10) << 4) | ((s->cr[0x09] & 0x40) << 3);
    const int pan_all = c256 ? (s->ar[0x13] >> 1) & 3 : (s->ar[0x13] & 7);
    const int pan_split = (s->ar[0x10] & 0x20) ? 0 : pan_all;
    const uint32_t row_bytes = c256 ? (uint32_t)pw + 4 : (uint32_t)pw / 2 + 4;   // + one more group for panning

    uint32_t pairs[256];   // two 16-colour pixels (the low nibble first) -> two RGB565 pixels
    if (!c256) {
        if (!nat_expand[0x80]) {
            for (int b = 0; b < 256; b++) {
                uint32_t e = 0;
                for (int i = 0; i < 8; i++) e |= ((b >> (7 - i)) & 1u) << (4 * i);
                nat_expand[b] = e;
            }
        }
        for (int i = 0; i < 256; i++) pairs[i] = pal[i & 15] | (pal[i >> 4] << 16);
    }
    const uint32_t mask32 = plane_mask * 0x11111111u;

    int changed = 0;
    uint32_t addr = start;
    int split_y = -1;
    for (int y = 0; y < h; y++, addr += line_offset) {
        int pan = pan_all;
        if (split_y < 0 && (uint32_t)(y * ly) > line_compare) { split_y = y; addr = 0; }
        if (split_y >= 0) pan = pan_split;
        const uint8_t* src = nat_src(s, addr, row_bytes);
        uint32_t hash = t386_mix(0x811C9DC5u ^ (uint32_t)pan, src, row_bytes - (pan ? 0 : 4));
        dirty[y] = hash != nat_row_hash[y];
        if (!dirty[y]) continue;
        nat_row_hash[y] = hash;
        ++changed;
        uint16_t* d = dst + y * pw;
        if (c256) {
            src += pan;
            uint32_t* d32 = (uint32_t*)d;
            for (int x = 0; x < pw / 2; x++) d32[x] = pal[src[2 * x]] | (pal[src[2 * x + 1]] << 16);
        } else {
            uint32_t tmp[(720 + 8) / 2];
            uint32_t* d32 = pan ? tmp : (uint32_t*)d;
            const int groups = pw / 8 + (pan ? 1 : 0);
            for (int g = 0; g < groups; g++, src += 4) {
                const uint32_t k = (nat_expand[src[0]] | (nat_expand[src[1]] << 1) | (nat_expand[src[2]] << 2) |
                                    (nat_expand[src[3]] << 3)) & mask32;
                d32[4 * g + 0] = pairs[k & 0xff];
                d32[4 * g + 1] = pairs[(k >> 8) & 0xff];
                d32[4 * g + 2] = pairs[(k >> 16) & 0xff];
                d32[4 * g + 3] = pairs[k >> 24];
            }
            if (pan) memcpy(d, (uint16_t*)tmp + pan, pw * 2);
        }
    }
    out->w = pw;
    out->h = h;
    out->dx = dx;
    out->ly = ly;
    out->changed = changed;
    out->pan = pan_all;
    out->split_y = split_y;
    return true;
}

// ---- fast video memory access
// The accesses games make most, straight from the CPU (t386_fast.h's fast_io_read/write: the fast interpreter and
// translated code) instead of through pc.c's iomem callbacks and vga_mem_write/read byte by byte. Only the
// A0000-AFFFF window (memory map mode 1) in chain-4 mode (mode 13h), or planar (EGA modes, mode X) with
// - writes: write mode 0 as a plain store (no rotation, logical operation, set/reset or bit mask), or write mode 1
//   (the latches, as copied by EGA games' tile drawing);
// - reads: read mode 0.
// Anything else returns 0 and goes the usual way. off is the offset from A0000; t386_vga is set once the PC exists
// (and cleared to compare: serial "vgafast 0"), then t386_vga_fw_update() called.
VGAState* t386_vga;

#include "t386_vga_fw.h"
struct T386VgaFw t386_vga_fw;

void t386_vga_fw_update(void) {
    VGAState* s = t386_vga;
    struct T386VgaFw f = {T386_FW_NONE, 0, NULL, NULL, 0, NULL};
    if (s && ((s->gr[VGA_GFX_MISC] >> 2) & 3) == 1 && s->bank_offset == 0 && s->vga_ram_size >= 0x40000) {
        const uint8_t mode = s->gr[VGA_GFX_MODE], pm = s->sr[VGA_SEQ_PLANE_WRITE] & 15;
        f.ram = (uint8_t*)s->vga_ram;
        f.latch = &s->latch;
        f.wm = mask16[pm];
        f.pm = pm;
        if (s->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M)
            f.kind = pm == 15 ? T386_FW_LINEAR : T386_FW_CHAIN4;
        else if (!(mode & 0x10) && (mode & 3) == 0 && !s->gr[VGA_GFX_DATA_ROTATE] && !s->gr[VGA_GFX_SR_ENABLE] &&
                 s->gr[VGA_GFX_BIT_MASK] == 0xff) {
            f.kind = T386_FW_PLANAR;
            if (pm && !(pm & (pm - 1))) {   // one plane
                f.kind = T386_FW_PLANE1;
                f.plane1 = f.ram + __builtin_ctz(pm);
            }
        }
        else if (!(mode & 0x10) && (mode & 3) == 1)
            f.kind = T386_FW_LATCH;
    }
    t386_vga_fw = f;
}

int t386_vga_write_fast(uint32_t off, int size, uint32_t v) {
    VGAState* s = t386_vga;
    if (!s || ((s->gr[VGA_GFX_MISC] >> 2) & 3) != 1 || off + size > 0x10000) return 0;
    off += s->bank_offset;
    const uint8_t mm = s->sr[VGA_SEQ_MEMORY_MODE], mode = s->gr[VGA_GFX_MODE], pm = s->sr[VGA_SEQ_PLANE_WRITE];
    if (mm & VGA_SR04_CHN_4M) {
        if (off + size > (uint32_t)s->vga_ram_size) return 0;
        for (int i = 0; i < size; i++, v >>= 8)
            if (pm & (1 << ((off + i) & 3))) s->vga_ram[off + i] = v;
        return 1;
    }
    if (mode & 0x10 || (off + size) * 4 > (uint32_t)s->vga_ram_size) return 0;   // odd/even (text)
    const uint32_t wm = mask16[pm & 15];
    uint32_t* p = (uint32_t*)s->vga_ram + off;
    switch (mode & 3) {
    case 0:
        if (s->gr[VGA_GFX_DATA_ROTATE] || s->gr[VGA_GFX_SR_ENABLE] || s->gr[VGA_GFX_BIT_MASK] != 0xff) return 0;
        for (int i = 0; i < size; i++, v >>= 8) p[i] = (p[i] & ~wm) | ((v & 0xff) * 0x01010101u & wm);
        return 1;
    case 1:
        for (int i = 0; i < size; i++) p[i] = (p[i] & ~wm) | (s->latch & wm);
        return 1;
    }
    return 0;
}

int t386_vga_read_fast(uint32_t off, int size, uint32_t* out) {
    VGAState* s = t386_vga;
    if (!s || ((s->gr[VGA_GFX_MISC] >> 2) & 3) != 1 || off + size > 0x10000) return 0;
    off += s->bank_offset;
    const uint8_t mm = s->sr[VGA_SEQ_MEMORY_MODE], mode = s->gr[VGA_GFX_MODE];
    uint32_t v = 0;
    if (mm & VGA_SR04_CHN_4M) {
        if (off + size > (uint32_t)s->vga_ram_size) return 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)s->vga_ram[off + i] << (8 * i);
    } else {
        if (mode & 0x18 || (off + size) * 4 > (uint32_t)s->vga_ram_size) return 0;   // odd/even, read mode 1
        const int plane = s->gr[VGA_GFX_PLANE_READ];
        const uint32_t* p = (const uint32_t*)s->vga_ram + off;
        for (int i = 0; i < size; i++) {
            s->latch = p[i];
            v |= GET_PLANE(s->latch, plane) << (8 * i);
        }
    }
    *out = v;
    return 1;
}

// vga_step for a read of the status register (t386_pc.c). Its deadlines compare in half the range of the 32-bit
// microsecond clock: after some 36 minutes without one (a paused guest), the last would look that far ahead, and the
// retrace stand still as long. Nothing is more than an interval ahead: one that is starts from now.
int t386_vga_step(VGAState* s) {
    if ((int32_t)(s->retrace_time - get_uticks()) > RETRACE_INTERVAL_US) s->retrace_time = get_uticks();
    return vga_step(s);
}
