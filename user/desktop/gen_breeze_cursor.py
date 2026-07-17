#!/usr/bin/env python3
"""Generate desktop_cursor_breeze.c from KDE Breeze default cursor SVG.

Requires ImageMagick (convert) to render SVG to ARGB8888 raster.
Usage: gen_breeze_cursor.py <output.c> [<breeze-dir>]
"""
import os
import subprocess
import sys
import tempfile
from PIL import Image

OUT = sys.argv[1]
BREEZE_DIR = sys.argv[2] if len(sys.argv) > 2 else "external/breeze"
SVG = os.path.join(BREEZE_DIR, "cursors", "Breeze", "src", "svg", "default.svg")
HOTSPOT_X = 4
HOTSPOT_Y = 4

if not os.path.exists(SVG):
    print(f"error: Breeze cursor SVG not found: {SVG}", file=sys.stderr)
    print("Clone KDE Breeze or set BREEZE_DIR to the cursors source tree:", file=sys.stderr)
    print("  git clone --depth 1 --filter=blob:none https://invent.kde.org/plasma/breeze.git external/breeze",
          file=sys.stderr)
    sys.exit(1)

with tempfile.TemporaryDirectory() as tmp:
    png = os.path.join(tmp, "default.png")
    subprocess.run(
        ["convert", "-density", "96", "-background", "none", SVG, "PNG32:" + png],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    img = Image.open(png).convert("RGBA")
    w, h = img.size
    pixels = list(img.getdata())

with open(OUT, "w") as f:
    f.write("/* Breeze cursor: breeze_cursor_default */\n")
    f.write("/* Rendered from KDE Breeze, LGPL-3.0-or-later / GPL-2.0-or-later */\n")
    f.write("/* This is a generated file; do not edit or commit to source control. */\n")
    f.write(f"/* Hotspot: {HOTSPOT_X},{HOTSPOT_Y} */\n\n")
    f.write("#include \"lvgl.h\"\n\n")
    f.write(f"static const uint32_t breeze_cursor_default_pixels[{w} * {h}] = {{\n")
    for i, (r, g, b, a) in enumerate(pixels):
        if i % 4 == 0:
            f.write("    ")
        val = (a << 24) | (r << 16) | (g << 8) | b
        f.write(f"0x{val:08x}U")
        if i != len(pixels) - 1:
            f.write(", ")
        if i % 4 == 3:
            f.write("\n")
    if len(pixels) % 4 != 0:
        f.write("\n")
    f.write(f"}};\n\n")
    f.write(f"const lv_image_dsc_t breeze_cursor_default_image = {{\n")
    f.write(f"    .header = {{\n")
    f.write(f"        .cf = LV_COLOR_FORMAT_ARGB8888,\n")
    f.write(f"        .w = {w},\n")
    f.write(f"        .h = {h},\n")
    f.write(f"        .stride = {w} * sizeof(uint32_t),\n")
    f.write(f"    }},\n")
    f.write(f"    .data_size = sizeof(breeze_cursor_default_pixels),\n")
    f.write(f"    .data = (const uint8_t *)breeze_cursor_default_pixels,\n")
    f.write(f"}};\n")

print(f"Generated {OUT}: {w}x{h} from {SVG}")
