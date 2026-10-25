// t386_vga.c's renderer for the VGA's 16- and 256-colour graphics modes at their own size (see there).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int w, h;      // pixels
    int dx, ly;    // dots per pixel across, scan lines per pixel line down (the shape on a monitor)
    int changed;   // lines drawn this time
    int pan;       // pixel panning (pixels), and the first line of the split screen's lower part (-1: none)
    int split_y;
    int bpp;       // a VBE mode's bits a pixel (0: a VGA mode)
} T386Native;

#ifdef __cplusplus
extern "C" {
#endif
bool t386_vga_render_native(struct VGAState* s, uint16_t* dst, uint8_t* dirty, int max_w, int max_h, bool force,
                            T386Native* out);
// The size of the picture tiny386's vga_refresh draws, centred in its frame buffer (text: the characters' dots;
// graphics: after a refresh), or false
bool t386_vga_picture(struct VGAState* s, int* w, int* h);
// The largest VBE mode the BIOS offers (0: tiny386's frame buffer's size)
extern int t386_vbe_max_w, t386_vbe_max_h;
#ifdef __cplusplus
}
#endif
