#!/usr/bin/env python3
"""Make firmware/tab5_386/src/px_fonts.c, the settings panel's pixel fonts: LVGL's UNSCII 8 (public domain, an 8 x 8
font in the style of the old system fonts; lv_font_unscii_8.c in the LVGL library) with every pixel made a kx x ky
block, taller than wide as text-mode pixels were on 4:3 monitors: crisp large text on the Tab5's screen. Symbols the font lacks (LVGL's icons) come from Montserrat.

    tools/fonts/px_fonts.py [path/to/lv_font_unscii_8.c]
"""
import os, re, sys

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Arduino/libraries/lvgl/src/font/lv_font_unscii_8.c")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../firmware/tab5_386/src/px_fonts.c")
FONTS = [("px_font_24", 2, 3, "lv_font_montserrat_20"), ("px_font_32", 3, 4, "lv_font_montserrat_28")]   # (16 x 24, 24 x 32)

text = open(SRC).read()
bm = text[text.index("glyph_bitmap[] = {"):]
bm = bm[:bm.index("};")]
bitmap = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", bm)]
glyphs = [tuple(int(v) for v in m) for m in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", text)]
assert len(glyphs) == 97, len(glyphs)   # (id 0, then 0x20-0x7f)
line_height = int(re.search(r"\.line_height = (\d+)", text).group(1))


def bits(index, n):
    for i in range(n):
        b = index * 8 + i
        yield (bitmap[b >> 3] >> (7 - (b & 7))) & 1


out = ["// Made by tools/fonts/px_fonts.py from LVGL's lv_font_unscii_8.c (UNSCII by Viznut: public domain), each pixel",
       "// a kx x ky block: the settings panel's pixel fonts (settings_ui.h). Do not edit.",
       "#include <lvgl.h>", ""]
for name, kx, ky, fallback in FONTS:
    data, dsc = [], ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"]
    for (index, adv, w, h, ox, oy) in glyphs[1:]:
        px = list(bits(index, w * h))
        big = []
        for y in range(h * ky):
            for x in range(w * kx):
                big.append(px[(y // ky) * w + x // kx])
        start = len(data)
        for i in range(0, len(big), 8):
            chunk = big[i:i + 8] + [0] * (8 - len(big[i:i + 8]))
            data.append(int("".join(map(str, chunk)), 2))
        dsc.append(f"    {{.bitmap_index = {start}, .adv_w = {adv * kx}, .box_w = {w * kx}, .box_h = {h * ky}, "
                   f".ofs_x = {ox * kx}, .ofs_y = {oy * ky}}}")
    out.append(f"static const uint8_t {name}_bitmap[] = {{")
    for i in range(0, len(data), 24):
        out.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    out += ["};", f"static const lv_font_fmt_txt_glyph_dsc_t {name}_glyphs[] = {{", ",\n".join(dsc), "};",
            f"static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{",
            "    {.range_start = 32, .range_length = 96, .glyph_id_start = 1, .unicode_list = NULL, .glyph_id_ofs_list = NULL,",
            "     .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}", "};",
            f"static const lv_font_fmt_txt_dsc_t {name}_dsc = {{",
            f"    .glyph_bitmap = {name}_bitmap, .glyph_dsc = {name}_glyphs, .cmaps = {name}_cmaps, .kern_dsc = NULL,",
            "    .kern_scale = 0, .cmap_num = 1, .bpp = 1, .kern_classes = 0, .bitmap_format = 0,", "};",
            f"const lv_font_t {name} = {{",
            "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt, .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
            f"    .line_height = {line_height * ky}, .base_line = 0, .subpx = LV_FONT_SUBPX_NONE,",
            "    .underline_position = 0, .underline_thickness = 0, .static_bitmap = 1,",
            f"    .dsc = &{name}_dsc, .fallback = &{fallback}, .user_data = NULL,", "};", ""]
open(OUT, "w").write("\n".join(out))
print("wrote", os.path.normpath(OUT))
