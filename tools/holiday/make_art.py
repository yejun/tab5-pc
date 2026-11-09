#!/usr/bin/env python3
"""Make firmware/tab5_386/holiday_art.h, the side panels' holiday decorations (side_ui.h), from art.html.

Chromium draws art.html#overlay on a transparent 320 x 720 page (the left panel, then the right); each panel's
picture is cut into pieces (runs of rows with something drawn, each cropped to what is drawn) kept as LVGL
RGB565A8 images. The hold ring's candy-cane stripes (an image the ring's arc shows) are made here.

    tools/holiday/make_art.py [--chromium PATH] [--png out.png]   (needs numpy and Pillow)
"""
import argparse
import os
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "firmware", "tab5_386", "holiday_art.h")
GAP = 6   # rows with nothing drawn that still keep two runs one piece


def render(chromium):
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, "art.png")
        subprocess.run([chromium, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                        "--default-background-color=00000000", "--window-size=320,720", "--virtual-time-budget=2000",
                        f"--user-data-dir={tmp}/prof", f"--screenshot={png}",
                        "file://" + os.path.join(HERE, "art.html") + "#overlay"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return np.array(Image.open(png).convert("RGBA"))


def pieces(rgba):
    a = rgba[:, :, 3]
    a[a < 4] = 0
    rows = np.flatnonzero(a.any(axis=1))
    runs, start, last = [], None, None
    for y in rows:
        if start is None:
            start = y
        elif y - last > GAP:
            runs.append((start, last))
            start = y
        last = y
    if start is not None:
        runs.append((start, last))
    for y0, y1 in runs:
        cols = np.flatnonzero(a[y0:y1 + 1].any(axis=0))
        x0, x1 = cols[0], cols[-1]
        yield x0, y0, rgba[y0:y1 + 1, x0:x1 + 1]


def rgb565(rgb):
    r, g, b = (rgb[..., i].astype(np.uint32) for i in range(3))
    return (((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)).astype("<u2")


def cane(size=78):
    """the hold ring's colours: red and white stripes across it, as a candy cane's (RGB565, the arc masks it)"""
    y, x = np.mgrid[0:size, 0:size]
    red = ((x + y) // 6) % 2 == 0
    rgb = np.where(red[..., None], np.array([0xE5, 0x30, 0x38]), np.array([0xF3, 0xF7, 0xFB]))
    return rgb565(rgb)


def c_bytes(data):
    hexes = [f"0x{b:02x}" for b in data]
    return "\n".join("  " + ",".join(hexes[i:i + 24]) + "," for i in range(0, len(hexes), 24))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chromium", default=shutil.which("chromium") or shutil.which("google-chrome-stable"))
    ap.add_argument("--png", help="also save the rendered page")
    args = ap.parse_args()
    rgba = render(args.chromium)
    if args.png:
        Image.fromarray(rgba).save(args.png)
    out = ["// The side panels' holiday decorations (side_ui.h). Made by tools/holiday/make_art.py from",
           "// tools/holiday/art.html: don't edit. Pieces of each panel's decorations (LVGL RGB565A8 images: the",
           "// pixels, then their alpha) where they go, and the hold ring's candy-cane stripes.",
           "#pragma once", "",
           "struct HoliPiece { int8_t panel; int16_t x, y; const lv_image_dsc_t* img; };", ""]
    table, total = [], 0
    for panel in range(2):
        for x, y, px in pieces(rgba[:, panel * 160:(panel + 1) * 160].copy()):
            n = len(table)
            h, w = px.shape[:2]
            data = rgb565(px[..., :3]).tobytes() + px[..., 3].astype(np.uint8).tobytes()
            total += len(data)
            out += [f"static const uint8_t holi_px{n}[] = {{", c_bytes(data), "};",
                    f"static const lv_image_dsc_t holi_img{n} = {{",
                    f"  {{LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565A8, 0, {w}, {h}, {w * 2}, 0}},",
                    f"  sizeof(holi_px{n}), holi_px{n}, nullptr, nullptr}};", ""]
            table.append(f"  {{{panel}, {x}, {y}, &holi_img{n}}},")
    c = cane()
    out += ["static const uint8_t holi_cane_px[] = {", c_bytes(c.tobytes()), "};",
            "static const lv_image_dsc_t holi_cane = {",
            f"  {{LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565, 0, {c.shape[1]}, {c.shape[0]}, {c.shape[1] * 2}, 0}},",
            "  sizeof(holi_cane_px), holi_cane_px, nullptr, nullptr};", "",
            "static const HoliPiece holi_pieces[] = {", *table, "};", ""]
    with open(OUT, "w") as f:
        f.write("\n".join(out))
    print(f"{OUT}: {len(table)} pieces, {total} bytes of pictures")


if __name__ == "__main__":
    main()
