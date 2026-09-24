#!/usr/bin/env python3
"""Render DejaVu TrueType fonts into ClaudeOS anti-aliased bitmap fonts (.fnt).

Format (little endian):
  header (24 bytes): "CFNT", u16 version=1, u16 count, i16 size, i16 ascent, i16 descent,
                     i16 line_height, u16 flags (1 = monospace), u16 reserved, u32 bitmap_offset
  glyphs (20 bytes each, sorted by codepoint):
                     u32 cp, i16 bx, i16 by, u16 w, u16 h, u16 advance (1/64 px), u16 pad, u32 offset
  bitmap: 8-bit alpha, w*h bytes per glyph

bx = left offset of the bitmap from the pen position, by = rows above the baseline of the
bitmap's top row.  The DejaVu fonts are under the Bitstream Vera / DejaVu license.
"""
import os, struct, sys
from PIL import Image, ImageDraw, ImageFont

FONT_DIR = "/usr/share/fonts/TTF"
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "assets", "fonts")

BASE = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x20AC,
         0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x221A, 0x03C0, 0x221E, 0x2248, 0x2260,
         0x2264, 0x2265, 0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x2713, 0x2715, 0x2716, 0x2605, 0x2606,
         0x266A, 0x266B, 0x23F8, 0x23F5, 0x23F9, 0x25CF, 0x25CB, 0x00B7]
BOX = list(range(0x2500, 0x2580)) + list(range(0x2580, 0x25A0))

FONTS = [
    # name, file, px size, mono, extra glyph sets
    ("ui", "DejaVuSans.ttf", 12, False, EXTRA),
    ("ui-bold", "DejaVuSans-Bold.ttf", 12, False, EXTRA),
    ("ui-large", "DejaVuSans.ttf", 15, False, EXTRA),
    ("title", "DejaVuSans-Bold.ttf", 18, False, EXTRA),
    ("display", "DejaVuSans-ExtraLight.ttf", 32, False, EXTRA),
    ("big-bold", "DejaVuSans-Bold.ttf", 26, False, EXTRA),
    ("mono", "DejaVuSansMono.ttf", 13, True, EXTRA + BOX),
    ("mono-bold", "DejaVuSansMono-Bold.ttf", 13, True, EXTRA + BOX),
]


def gamma(a):
    # slightly heavier strokes look better when blending in sRGB space
    return int(round(255 * ((a / 255.0) ** 0.80)))


GAMMA_LUT = bytes(gamma(i) for i in range(256))


def render(name, file, size, mono, extra):
    font = ImageFont.truetype(os.path.join(FONT_DIR, file), size)
    ascent, descent = font.getmetrics()
    cps = sorted(set(BASE + extra))
    glyphs, bitmap = [], bytearray()
    cell = round(font.getlength("M")) if mono else 0
    line_height = ascent + descent
    for cp in cps:
        ch = chr(cp)
        # skip glyphs the font does not have (they render as .notdef boxes)
        if cp > 0xFF and font.getmask(ch).getbbox() is None and cp not in (0x20,):
            try:
                if font.font.getsize(ch)[0][0] == 0:
                    continue
            except Exception:
                pass
        l, t, r, b = font.getbbox(ch, anchor="ls")
        w, h = max(0, r - l), max(0, b - t)
        adv = cell * 64 if mono else int(round(font.getlength(ch) * 64))
        off = len(bitmap)
        if w > 0 and h > 0:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
            bitmap += img.tobytes().translate(GAMMA_LUT)
        glyphs.append((cp, l, -t, w, h, adv, off))
    count = len(glyphs)
    header_size, glyph_size = 24, 20
    bitmap_offset = header_size + glyph_size * count
    out = bytearray()
    out += struct.pack("<4sHHhhhhHHI", b"CFNT", 1, count, size, ascent, descent, line_height,
                       1 if mono else 0, 0, bitmap_offset)
    for cp, bx, by, w, h, adv, off in glyphs:
        out += struct.pack("<IhhHHHHI", cp, bx, by, w, h, adv, 0, off)
    out += bitmap
    path = os.path.join(OUT, name + ".fnt")
    with open(path, "wb") as f:
        f.write(out)
    print("%-10s %2dpx asc=%d desc=%d glyphs=%d %6d bytes%s" % (name, size, ascent, descent, count, len(out),
          " cell=%d" % cell if mono else ""))


os.makedirs(OUT, exist_ok=True)
for spec in FONTS:
    render(*spec)
