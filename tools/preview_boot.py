#!/usr/bin/env python3
"""Render the FM-1 boot screen to a PNG using the real font from app/screen.c.

Run from the repo root:  python3 tools/preview_boot.py boot-preview.png
Needs Pillow (pip install pillow). Mirrors boot_draw(); update both together.
"""
import re, sys
from pathlib import Path
from PIL import Image

src = Path(__file__).resolve().parent.parent / "app" / "screen.c"
c = src.read_text()
version = re.search(r'FM1_RELEASE_VERSION "([^"]+)"',
                    (src.parent / "version.h").read_text()).group(1)

def block(name):
    body = re.search(name + r"[^=]*=\s*\{(.*?)\};", c, re.S).group(1)
    return [[int(n, 0) for n in re.findall(r"0x[0-9a-fA-F]+|\d+", g)]
            for g in re.findall(r"\{([^{}]*)\}", body)]

font = block(r"static const uint8_t font\[36\]\[5\]")
punct = {row[0]: row[1:] for row in block(r"static const uint8_t punctuation\[\]\[6\]")}
defs = dict(re.findall(r'#define (BOOT_\w+) "([^"]*)"', c))
enum = {k: int(v, 0) for k, v in re.findall(r"(\w+) = (0x[0-9a-fA-F]+)", c.split("};")[0])}
colors = [int(v, 16) for v in re.search(r"colors\[8\] = \{([^}]*)\}", c).group(1).replace("0x", "").split(", ")]

def rgb(v):
    return ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)

img = Image.new("RGB", (240, 240), rgb(enum["BACK"]))
px = img.load()

def fill(x, y, w, h, col):
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if 0 <= xx < 240 and 0 <= yy < 240: px[xx, yy] = rgb(col)

def glyph(ch):
    if ch.isdigit(): return font[ord(ch) - 48]
    if ch.isalpha(): return font[10 + ord(ch.upper()) - 65]
    return punct.get(ord(ch), [0] * 5)

def text(x, y, s, scale, col):
    for ch in s:
        for cx, bits in enumerate(glyph(ch)):
            for cy in range(7):
                if bits >> cy & 1: fill(x + cx * scale, y + cy * scale, scale, scale, col)
        x += 6 * scale

center = lambda s, scale: (240 - 6 * scale * len(s)) // 2
text(center(defs["BOOT_LINE_1"], 4), 36, defs["BOOT_LINE_1"], 4, enum["TEXT"])
text(center(defs["BOOT_LINE_2"], 4), 72, defs["BOOT_LINE_2"], 4, enum["ACCENT"])
text(center(version, 2), 116, version, 2, enum["DIM"])
text(center(defs["BOOT_FOOTER"], 1), 208, defs["BOOT_FOOTER"], 1, enum["DIM"])
for i, h in enumerate([8, 18, 30, 42, 42, 30, 18, 8]):  # fully loaded bars
    fill(58 + 16 * i, 166 - h // 2, 12, h, colors[i])

out = sys.argv[1] if len(sys.argv) > 1 else "boot-preview.png"
img.resize((480, 480), Image.NEAREST).save(out)
print("wrote", out)
