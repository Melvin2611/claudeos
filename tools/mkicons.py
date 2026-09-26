#!/usr/bin/env python3
"""Generate the ClaudeOS icon set (.icn = "CICN", u16 w, u16 h, ARGB32 pixels).

Colour icons are drawn at 256x256 and downsampled to 48/32/24/16 px.
Glyph icons (monochrome, white + alpha) are tinted by the window manager / libgui.
Cursors are drawn at their final size with a black outline.
"""
import math, os, struct, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "assets", "icons")
S = 256
SIZES = (48, 32, 24, 16)
FONT_BOLD = "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"
FONT_MONO = "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"


def save_icn(img, path):
    img = img.convert("RGBA")
    w, h = img.size
    data = bytearray(b"CICN" + struct.pack("<HH", w, h))
    px = img.tobytes()
    for i in range(0, len(px), 4):
        r, g, b, a = px[i], px[i + 1], px[i + 2], px[i + 3]
        data += bytes((b, g, r, a))
    with open(path, "wb") as f:
        f.write(data)


def canvas():
    return Image.new("RGBA", (S, S), (0, 0, 0, 0))


def hexc(s, a=255):
    s = s.lstrip("#")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), a)


def vgrad(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(g)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return g


def rrect_grad(img, box, r, top, bottom):
    x0, y0, x1, y1 = box
    grad = vgrad((x1 - x0, y1 - y0), top, bottom)
    mask = Image.new("L", grad.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, grad.size[0] - 1, grad.size[1] - 1], r, fill=255)
    img.paste(grad, (x0, y0), mask)


def shadow(img, box, r, strength=90, blur=10, dy=6):
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
    x0, y0, x1, y1 = box
    ImageDraw.Draw(sh).rounded_rectangle([x0, y0 + dy, x1, y1 + dy], r, fill=(0, 0, 0, strength))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    return Image.alpha_composite(sh, img)


def finish(img, name, sizes=SIZES):
    for s in sizes:
        small = img.resize((s, s), Image.LANCZOS)
        save_icn(small, os.path.join(OUT, "%s-%d.icn" % (name, s)))


def with_shadow(draw_fn, box=(24, 24, 232, 232), r=48):
    img = canvas()
    draw_fn(img)
    return shadow(img, box, r, 70, 8, 5)


# ----------------------------------------------------------------------------- colour icons
def icon_logo(img):
    rrect_grad(img, (16, 16, 240, 240), 56, hexc("#E88A68"), hexc("#C9603F"))
    d = ImageDraw.Draw(img)
    d.ellipse([60, 60, 196, 196], fill=(255, 255, 255, 255))
    d.ellipse([98, 98, 158, 158], fill=hexc("#D77252"))
    d.rectangle([128, 104, 200, 152], fill=hexc("#D77252"))


def icon_terminal(img):
    rrect_grad(img, (20, 28, 236, 228), 36, hexc("#3A3D46"), hexc("#1C1D22"))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([20, 28, 236, 70], 36, fill=hexc("#4A4E59"))
    d.rectangle([20, 52, 236, 70], fill=hexc("#4A4E59"))
    for i, c in enumerate(("#FF5F57", "#FEBC2E", "#28C840")):
        d.ellipse([40 + i * 28, 40, 58 + i * 28, 58], fill=hexc(c))
    d.line([(58, 106), (104, 140), (58, 174)], fill=hexc("#7EE787"), width=18, joint="curve")
    d.rectangle([118, 164, 190, 180], fill=(255, 255, 255, 235))


def icon_folder(img, front="#FFCB57", back="#E9A93A"):
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([24, 52, 116, 100], 18, fill=hexc(back))
    rrect_grad(img, (24, 72, 232, 220), 22, hexc(back), hexc(back))
    rrect_grad(img, (24, 92, 232, 222), 22, hexc(front), hexc("#F7B53B" if front == "#FFCB57" else front))
    d = ImageDraw.Draw(img)
    d.line([(40, 104), (216, 104)], fill=(255, 255, 255, 90), width=4)


def page(img, fold_color="#D9DEE7", body=("#FFFFFF", "#EEF1F6")):
    x0, y0, x1, y1 = 50, 20, 206, 236
    f = 48
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).polygon([(x0, y0 + 10), (x1 - f, y0), (x1, y0 + f), (x1, y1), (x0, y1)], fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(0.5))
    grad = vgrad((S, S), hexc(body[0]), hexc(body[1]))
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.polygon([(x1 - f, y0), (x1 - f, y0 + f), (x1, y0 + f)], fill=hexc(fold_color))
    d.line([(x0, y0 + 10), (x1 - f, y0), (x1, y0 + f), (x1, y1), (x0, y1), (x0, y0 + 10)], fill=hexc("#AEB6C4"), width=4)
    return d


def icon_file(img):
    page(img)


def icon_file_text(img):
    d = page(img)
    for i, y in enumerate(range(90, 210, 22)):
        d.rounded_rectangle([74, y, 182 - (40 if i % 3 == 2 else 0), y + 8], 4, fill=hexc("#8A96AB"))


def icon_editor(img):
    d = page(img)
    for i, y in enumerate(range(90, 200, 24)):
        d.rounded_rectangle([74, y, 170 - (36 if i % 2 else 0), y + 9], 4, fill=hexc("#4C8BF5"))
    # pencil
    pen = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    pd = ImageDraw.Draw(pen)
    pd.polygon([(150, 238), (140, 206), (212, 134), (238, 160), (166, 232)], fill=hexc("#F5A623"))
    pd.polygon([(212, 134), (226, 120), (252, 146), (238, 160)], fill=hexc("#E0628A"))
    pd.polygon([(150, 238), (140, 206), (166, 232)], fill=hexc("#F3D9A4"))
    pd.polygon([(146, 240), (142, 226), (156, 236)], fill=hexc("#333333"))
    img.alpha_composite(pen)


def icon_calc(img):
    rrect_grad(img, (40, 16, 216, 240), 30, hexc("#4B5060"), hexc("#2E3139"))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([60, 36, 196, 88], 12, fill=hexc("#BFE3C9"))
    d.text((180, 62), "42", fill=hexc("#1D3B26"), anchor="rm", font=ImageFont.truetype(FONT_BOLD, 36))
    for r in range(3):
        for c in range(4):
            col = "#F28C38" if c == 3 else "#6B7182"
            x, y = 60 + c * 36, 104 + r * 42
            d.rounded_rectangle([x, y, x + 28, y + 32], 8, fill=hexc(col))


def icon_paint(img):
    d = ImageDraw.Draw(img)
    pal = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    pd = ImageDraw.Draw(pal)
    pd.ellipse([16, 40, 230, 220], fill=hexc("#F1D7A8"))
    pd.ellipse([140, 150, 186, 196], fill=(0, 0, 0, 0))
    img.alpha_composite(pal)
    d = ImageDraw.Draw(img)
    for (x, y), c in zip([(60, 90), (104, 66), (156, 76), (190, 118), (66, 146)],
                         ["#E5484D", "#F5A623", "#30A46C", "#3E63DD", "#8E4EC6"]):
        d.ellipse([x - 20, y - 20, x + 20, y + 20], fill=hexc(c))
    d.line([(120, 200), (236, 20)], fill=hexc("#8B5A2B"), width=16)
    d.polygon([(108, 214), (118, 188), (134, 198)], fill=hexc("#2B2B2B"))


def icon_viewer(img):
    rrect_grad(img, (18, 36, 238, 220), 22, hexc("#6FB6FF"), hexc("#CBE6FF"))
    d = ImageDraw.Draw(img)
    d.ellipse([160, 60, 204, 104], fill=hexc("#FFD24C"))
    d.polygon([(18, 200), (90, 110), (150, 190), (180, 150), (238, 210), (238, 220), (18, 220)], fill=hexc("#3F9E5A"))
    d.polygon([(18, 220), (18, 200), (70, 160), (130, 220)], fill=hexc("#2F7D45"))
    d.rounded_rectangle([18, 36, 238, 220], 22, outline=(255, 255, 255, 255), width=8)


def icon_music(img):
    grad = vgrad((S, S), hexc("#B267E6"), hexc("#5B3CC4"))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([16, 16, 240, 240], fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.ellipse([74, 150, 124, 190], fill=(255, 255, 255, 255))
    d.ellipse([150, 132, 200, 172], fill=(255, 255, 255, 255))
    d.rectangle([108, 70, 122, 172], fill=(255, 255, 255, 255))
    d.rectangle([184, 52, 198, 154], fill=(255, 255, 255, 255))
    d.polygon([(108, 70), (198, 52), (198, 80), (108, 98)], fill=(255, 255, 255, 255))


def icon_taskmgr(img):
    rrect_grad(img, (18, 30, 238, 210), 24, hexc("#26303F"), hexc("#141A23"))
    d = ImageDraw.Draw(img)
    for x in range(38, 220, 30):
        d.line([(x, 46), (x, 194)], fill=(255, 255, 255, 25), width=2)
    pts = [(34, 170), (70, 150), (100, 160), (130, 100), (160, 124), (190, 70), (222, 90)]
    d.line(pts, fill=hexc("#4ADE80"), width=12, joint="curve")
    d.rounded_rectangle([98, 214, 158, 232], 6, fill=hexc("#8A93A6"))


def icon_settings(img):
    cx, cy = 128, 128
    pts = []
    teeth = 8
    for i in range(teeth * 4):
        a = i * math.pi * 2 / (teeth * 4)
        r = 112 if (i % 4) in (0, 1) else 86
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    grad = vgrad((S, S), hexc("#A7B0BF"), hexc("#626B7B"))
    mask = Image.new("L", (S, S), 0)
    md = ImageDraw.Draw(mask)
    md.polygon(pts, fill=255)
    md.ellipse([cx - 40, cy - 40, cx + 40, cy + 40], fill=0)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.ellipse([cx - 58, cy - 58, cx + 58, cy + 58], outline=hexc("#D5DAE3"), width=10)


def icon_about(img):
    grad = vgrad((S, S), hexc("#4C9AFF"), hexc("#1E5FD8"))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([16, 16, 240, 240], fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.ellipse([112, 52, 144, 84], fill=(255, 255, 255, 255))
    d.rounded_rectangle([112, 102, 144, 200], 10, fill=(255, 255, 255, 255))


def icon_mines(img):
    rrect_grad(img, (24, 24, 232, 232), 30, hexc("#C9CFD9"), hexc("#98A1B0"))
    d = ImageDraw.Draw(img)
    cx, cy = 128, 132
    for i in range(8):
        a = i * math.pi / 4
        d.line([(cx + 30 * math.cos(a), cy + 30 * math.sin(a)), (cx + 80 * math.cos(a), cy + 80 * math.sin(a))],
               fill=hexc("#1F1F1F"), width=16)
    d.ellipse([cx - 56, cy - 56, cx + 56, cy + 56], fill=hexc("#1F1F1F"))
    d.ellipse([cx - 32, cy - 34, cx - 10, cy - 12], fill=(255, 255, 255, 220))


def icon_snake(img):
    rrect_grad(img, (24, 24, 232, 232), 40, hexc("#1F3B2B"), hexc("#12241A"))
    d = ImageDraw.Draw(img)
    d.line([(64, 190), (64, 120), (128, 120), (128, 70), (190, 70)], fill=hexc("#4ADE80"), width=36, joint="curve")
    d.ellipse([172, 52, 208, 88], fill=hexc("#4ADE80"))
    d.ellipse([186, 62, 196, 72], fill=hexc("#10231A"))
    d.ellipse([160, 160, 200, 200], fill=hexc("#EF4444"))
    d.line([(180, 160), (188, 146)], fill=hexc("#7C4A1E"), width=6)


def icon_tetris(img):
    d = ImageDraw.Draw(img)
    def block(x, y, c):
        rrect_grad(img, (x, y, x + 60, y + 60), 10, hexc(c), tuple(int(v * 0.75) for v in hexc(c)[:3]) + (255,))
    for (x, y, c) in [(38, 38, "#A855F7"), (98, 38, "#A855F7"), (158, 38, "#A855F7"), (98, 98, "#A855F7"),
                      (38, 158, "#22D3EE"), (98, 158, "#F59E0B"), (158, 158, "#F59E0B"), (158, 98, "#F59E0B")]:
        block(x, y, c)


def icon_file_image(img):
    page(img)
    d = ImageDraw.Draw(img)
    d.rectangle([72, 100, 184, 196], fill=hexc("#9ED0FF"))
    d.polygon([(72, 196), (110, 140), (140, 180), (160, 160), (184, 196)], fill=hexc("#3F9E5A"))
    d.ellipse([150, 110, 172, 132], fill=hexc("#FFD24C"))


def icon_file_audio(img):
    page(img)
    d = ImageDraw.Draw(img)
    d.ellipse([84, 168, 118, 196], fill=hexc("#7C4DDB"))
    d.ellipse([140, 156, 174, 184], fill=hexc("#7C4DDB"))
    d.rectangle([108, 100, 118, 184], fill=hexc("#7C4DDB"))
    d.rectangle([164, 88, 174, 172], fill=hexc("#7C4DDB"))
    d.polygon([(108, 100), (174, 88), (174, 108), (108, 120)], fill=hexc("#7C4DDB"))


def icon_file_exec(img):
    rrect_grad(img, (28, 44, 228, 212), 20, hexc("#5B8DEF"), hexc("#3565CF"))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([28, 44, 228, 84], 20, fill=hexc("#284C9E"))
    d.rectangle([28, 64, 228, 84], fill=hexc("#284C9E"))
    d.rectangle([48, 104, 208, 192], fill=(255, 255, 255, 60))
    d.line([(66, 130), (92, 148), (66, 166)], fill=(255, 255, 255, 255), width=10)
    d.line([(104, 170), (150, 170)], fill=(255, 255, 255, 255), width=10)


def icon_drive(img):
    rrect_grad(img, (20, 70, 236, 190), 22, hexc("#C8CED8"), hexc("#8F98A7"))
    d = ImageDraw.Draw(img)
    d.line([(36, 150), (220, 150)], fill=(255, 255, 255, 120), width=4)
    d.ellipse([186, 160, 204, 178], fill=hexc("#4ADE80"))
    for x in range(44, 150, 20):
        d.line([(x, 162), (x, 176)], fill=hexc("#6B7483"), width=6)


def icon_home(img):
    d = ImageDraw.Draw(img)
    rrect_grad(img, (52, 110, 204, 226), 10, hexc("#F4F6FA"), hexc("#D5DBE5"))
    d = ImageDraw.Draw(img)
    d.polygon([(128, 26), (236, 122), (206, 122), (128, 54), (50, 122), (20, 122)], fill=hexc("#E5484D"))
    d.rounded_rectangle([108, 150, 148, 226], 6, fill=hexc("#8B5A2B"))


def icon_trash(img):
    d = ImageDraw.Draw(img)
    rrect_grad(img, (60, 70, 196, 232), 16, hexc("#AEB6C4"), hexc("#7D8697"))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([44, 44, 212, 70], 10, fill=hexc("#6B7483"))
    d.rounded_rectangle([104, 26, 152, 48], 8, fill=hexc("#6B7483"))
    for x in (96, 128, 160):
        d.line([(x, 96), (x, 206)], fill=(255, 255, 255, 140), width=10)


def icon_network(img):
    grad = vgrad((S, S), hexc("#34D399"), hexc("#0E9F6E"))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([16, 16, 240, 240], fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.ellipse([56, 56, 200, 200], outline=(255, 255, 255, 255), width=12)
    d.ellipse([100, 56, 156, 200], outline=(255, 255, 255, 255), width=10)
    d.line([(56, 128), (200, 128)], fill=(255, 255, 255, 255), width=10)


def icon_browser(img):
    # a blue globe with a compass needle
    grad = vgrad((S, S), hexc("#60A5FA"), hexc("#1D4ED8"))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([16, 16, 240, 240], fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    w = (255, 255, 255, 170)
    d.ellipse([40, 40, 216, 216], outline=w, width=8)
    d.ellipse([88, 40, 168, 216], outline=w, width=8)
    d.line([(40, 128), (216, 128)], fill=w, width=8)
    d.line([(56, 84), (200, 84)], fill=w, width=6)
    d.line([(56, 172), (200, 172)], fill=w, width=6)
    d.polygon([(128, 128), (176, 64), (150, 142)], fill=hexc("#F97316"))
    d.polygon([(128, 128), (80, 192), (106, 114)], fill=(255, 255, 255, 255))
    d.ellipse([118, 118, 138, 138], fill=hexc("#1E293B"))


def icon_clock(img):
    grad = vgrad((S, S), hexc("#F8FAFC"), hexc("#D5DBE5"))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([16, 16, 240, 240], fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.ellipse([16, 16, 240, 240], outline=hexc("#4C5566"), width=12)
    d.line([(128, 128), (128, 60)], fill=hexc("#2B2F38"), width=14)
    d.line([(128, 128), (180, 150)], fill=hexc("#E5484D"), width=10)
    d.ellipse([116, 116, 140, 140], fill=hexc("#2B2F38"))


COLOR_ICONS = {
    "logo": icon_logo, "terminal": icon_terminal, "folder": icon_folder, "files": icon_folder,
    "editor": icon_editor, "calculator": icon_calc, "paint": icon_paint, "viewer": icon_viewer,
    "music": icon_music, "taskmgr": icon_taskmgr, "settings": icon_settings, "about": icon_about,
    "mines": icon_mines, "snake": icon_snake, "tetris": icon_tetris, "file": icon_file,
    "file-text": icon_file_text, "file-image": icon_file_image, "file-audio": icon_file_audio,
    "file-exec": icon_file_exec, "drive": icon_drive, "home": icon_home, "trash": icon_trash,
    "network": icon_network, "clock": icon_clock, "browser": icon_browser,
}

# ----------------------------------------------------------------------------- glyphs (white, tinted at runtime)
W = (255, 255, 255, 255)
LW = 22   # line width at 256 px


def g_power(d):
    d.arc([48, 52, 208, 212], -60, 240, fill=W, width=LW)
    d.line([(128, 28), (128, 124)], fill=W, width=LW)


def g_restart(d):
    d.arc([48, 48, 208, 208], -20, 290, fill=W, width=LW)
    d.polygon([(206, 24), (216, 110), (140, 84)], fill=W)


def g_search(d):
    d.ellipse([36, 36, 164, 164], outline=W, width=LW)
    d.line([(150, 150), (224, 224)], fill=W, width=LW + 6)


def g_volume(d, waves=2):
    d.polygon([(30, 96), (80, 96), (140, 40), (140, 216), (80, 160), (30, 160)], fill=W)
    if waves >= 1:
        d.arc([110, 70, 190, 186], -50, 50, fill=W, width=LW - 4)
    if waves >= 2:
        d.arc([100, 30, 240, 226], -50, 50, fill=W, width=LW - 4)


def g_volume_mute(d):
    d.polygon([(30, 96), (80, 96), (140, 40), (140, 216), (80, 160), (30, 160)], fill=W)
    d.line([(168, 96), (232, 160)], fill=W, width=LW)
    d.line([(232, 96), (168, 160)], fill=W, width=LW)


def g_network(d):
    for i, r in enumerate((190, 130, 70)):
        d.arc([128 - r / 2 * 1.3, 190 - r, 128 + r / 2 * 1.3, 190 + r], 225, 315, fill=W, width=LW)
    d.ellipse([110, 196, 146, 232], fill=W)


def g_network_wired(d):
    d.rounded_rectangle([92, 20, 164, 84], 8, outline=W, width=LW - 4)
    d.rounded_rectangle([20, 172, 92, 236], 8, outline=W, width=LW - 4)
    d.rounded_rectangle([164, 172, 236, 236], 8, outline=W, width=LW - 4)
    d.line([(128, 84), (128, 128), (56, 128), (56, 172)], fill=W, width=LW - 6)
    d.line([(128, 128), (200, 128), (200, 172)], fill=W, width=LW - 6)


def g_network_off(d):
    g_network_wired(d)
    d.line([(30, 30), (226, 226)], fill=W, width=LW)


def arrow(d, pts):
    d.line(pts, fill=W, width=LW, joint="curve")


def g_back(d): arrow(d, [(220, 128), (40, 128)]); arrow(d, [(110, 58), (40, 128), (110, 198)])
def g_forward(d): arrow(d, [(36, 128), (216, 128)]); arrow(d, [(146, 58), (216, 128), (146, 198)])
def g_up(d): arrow(d, [(128, 220), (128, 40)]); arrow(d, [(58, 110), (128, 40), (198, 110)])
def g_down(d): arrow(d, [(128, 36), (128, 216)]); arrow(d, [(58, 146), (128, 216), (198, 146)])
def g_chev_down(d): arrow(d, [(52, 94), (128, 170), (204, 94)])
def g_chev_up(d): arrow(d, [(52, 162), (128, 86), (204, 162)])
def g_chev_right(d): arrow(d, [(94, 52), (170, 128), (94, 204)])
def g_chev_left(d): arrow(d, [(162, 52), (86, 128), (162, 204)])


def g_home(d):
    d.line([(24, 128), (128, 36), (232, 128)], fill=W, width=LW, joint="curve")
    d.line([(64, 110), (64, 220), (192, 220), (192, 110)], fill=W, width=LW, joint="curve")
    d.rectangle([108, 150, 148, 220], fill=W)


def g_refresh(d):
    d.arc([40, 40, 216, 216], 20, 330, fill=W, width=LW)
    d.polygon([(232, 60), (232, 140), (160, 110)], fill=W)


def g_folder(d):
    d.line([(24, 60), (100, 60), (124, 84), (232, 84), (232, 208), (24, 208), (24, 60)], fill=W, width=LW - 4, joint="curve")


def g_newfolder(d):
    g_folder(d)
    d.line([(128, 112), (128, 184)], fill=W, width=LW - 4)
    d.line([(92, 148), (164, 148)], fill=W, width=LW - 4)


def g_file(d):
    d.line([(52, 24), (160, 24), (212, 76), (212, 232), (52, 232), (52, 24)], fill=W, width=LW - 4, joint="curve")
    d.line([(160, 24), (160, 76), (212, 76)], fill=W, width=LW - 6)


def g_newfile(d):
    g_file(d)
    d.line([(132, 108), (132, 196)], fill=W, width=LW - 4)
    d.line([(88, 152), (176, 152)], fill=W, width=LW - 4)


def g_delete(d):
    d.line([(40, 64), (216, 64)], fill=W, width=LW)
    d.line([(100, 64), (108, 30), (148, 30), (156, 64)], fill=W, width=LW - 6)
    d.line([(64, 64), (80, 228), (176, 228), (192, 64)], fill=W, width=LW - 4, joint="curve")
    d.line([(112, 100), (112, 196)], fill=W, width=LW - 8)
    d.line([(144, 100), (144, 196)], fill=W, width=LW - 8)


def g_rename(d):
    d.polygon([(40, 216), (52, 164), (176, 40), (216, 80), (92, 204)], outline=W, width=LW - 4)
    d.line([(150, 66), (190, 106)], fill=W, width=LW - 6)


def g_save(d):
    d.line([(36, 36), (184, 36), (220, 72), (220, 220), (36, 220), (36, 36)], fill=W, width=LW - 4, joint="curve")
    d.rectangle([76, 36, 172, 96], outline=W, width=LW - 6)
    d.rectangle([72, 140, 184, 220], outline=W, width=LW - 6)


def g_open(d):
    d.line([(24, 200), (24, 60), (96, 60), (120, 84), (200, 84), (200, 112)], fill=W, width=LW - 4, joint="curve")
    d.polygon([(24, 204), (64, 116), (236, 116), (196, 204)], outline=W, width=LW - 4)


def g_copy(d):
    d.rounded_rectangle([84, 84, 220, 228], 14, outline=W, width=LW - 4)
    d.line([(172, 60), (172, 32), (36, 32), (36, 176), (64, 176)], fill=W, width=LW - 4)


def g_paste(d):
    d.rounded_rectangle([44, 44, 212, 232], 16, outline=W, width=LW - 4)
    d.rounded_rectangle([92, 24, 164, 68], 10, fill=W)


def g_cut(d):
    d.ellipse([28, 156, 100, 228], outline=W, width=LW - 6)
    d.ellipse([156, 156, 228, 228], outline=W, width=LW - 6)
    d.line([(84, 164), (190, 28)], fill=W, width=LW - 6)
    d.line([(172, 164), (66, 28)], fill=W, width=LW - 6)


def g_play(d): d.polygon([(64, 32), (224, 128), (64, 224)], fill=W)
def g_pause(d): d.rectangle([56, 40, 108, 216], fill=W); d.rectangle([148, 40, 200, 216], fill=W)
def g_stop(d): d.rounded_rectangle([48, 48, 208, 208], 16, fill=W)
def g_next(d): d.polygon([(40, 40), (168, 128), (40, 216)], fill=W); d.rectangle([180, 40, 216, 216], fill=W)
def g_prev(d): d.polygon([(216, 40), (88, 128), (216, 216)], fill=W); d.rectangle([40, 40, 76, 216], fill=W)
def g_close(d): d.line([(52, 52), (204, 204)], fill=W, width=LW); d.line([(204, 52), (52, 204)], fill=W, width=LW)
def g_check(d): d.line([(36, 132), (100, 196), (224, 60)], fill=W, width=LW + 4, joint="curve")
def g_plus(d): d.line([(128, 36), (128, 220)], fill=W, width=LW); d.line([(36, 128), (220, 128)], fill=W, width=LW)
def g_minus(d): d.line([(36, 128), (220, 128)], fill=W, width=LW)


def g_menu(d):
    for y in (60, 128, 196):
        d.line([(36, y), (220, y)], fill=W, width=LW)


def g_undo(d):
    d.arc([60, 60, 220, 220], 180, 90, fill=W, width=LW)
    d.polygon([(24, 140), (96, 140), (60, 90)], fill=W)


def g_redo(d):
    d.arc([36, 60, 196, 220], 90, 360, fill=W, width=LW)
    d.polygon([(232, 140), (160, 140), (196, 90)], fill=W)


def g_pencil(d):
    d.polygon([(40, 216), (52, 164), (176, 40), (216, 80), (92, 204)], fill=W)


def g_brush(d):
    d.line([(224, 32), (116, 140)], fill=W, width=LW)
    d.ellipse([40, 128, 128, 216], fill=W)
    d.polygon([(40, 180), (40, 232), (92, 232)], fill=W)


def g_eraser(d):
    d.polygon([(28, 168), (140, 56), (220, 136), (132, 224), (84, 224)], outline=W, width=LW - 4)
    d.line([(84, 112), (164, 192)], fill=W, width=LW - 6)
    d.line([(132, 224), (232, 224)], fill=W, width=LW - 6)


def g_line(d): d.line([(40, 216), (216, 40)], fill=W, width=LW)
def g_rect(d): d.rectangle([36, 64, 220, 192], outline=W, width=LW)
def g_ellipse(d): d.ellipse([28, 60, 228, 196], outline=W, width=LW)


def g_fill(d):
    d.polygon([(40, 120), (120, 40), (208, 128), (128, 208)], outline=W, width=LW - 4)
    d.polygon([(52, 124), (208, 128), (128, 208)], fill=W)
    d.ellipse([196, 160, 236, 232], fill=W)


def g_picker(d):
    d.line([(40, 216), (148, 108)], fill=W, width=LW)
    d.ellipse([140, 36, 220, 116], fill=W)
    d.line([(120, 80), (176, 136)], fill=W, width=LW + 8)


def g_text(d):
    d.line([(40, 48), (216, 48)], fill=W, width=LW + 4)
    d.line([(128, 48), (128, 220)], fill=W, width=LW + 4)


def g_zoom_in(d): g_search(d); d.line([(70, 100), (130, 100)], fill=W, width=LW - 6); d.line([(100, 70), (100, 130)], fill=W, width=LW - 6)
def g_zoom_out(d): g_search(d); d.line([(70, 100), (130, 100)], fill=W, width=LW - 6)


def g_flag(d):
    d.line([(80, 32), (80, 228)], fill=W, width=LW)
    d.polygon([(80, 32), (212, 76), (80, 120)], fill=W)


def g_grid(d):
    for x in (36, 144):
        for y in (36, 144):
            d.rounded_rectangle([x, y, x + 76, y + 76], 12, fill=W)


def g_list(d):
    for y in (48, 128, 208):
        d.ellipse([28, y - 16, 60, y + 16], fill=W)
        d.line([(88, y), (228, y)], fill=W, width=LW)


def g_computer(d):
    d.rounded_rectangle([24, 36, 232, 176], 14, outline=W, width=LW - 4)
    d.line([(128, 176), (128, 216)], fill=W, width=LW - 4)
    d.line([(72, 220), (184, 220)], fill=W, width=LW)


def g_image(d):
    d.rounded_rectangle([24, 44, 232, 212], 14, outline=W, width=LW - 4)
    d.polygon([(40, 196), (100, 116), (150, 176), (176, 146), (216, 196)], fill=W)
    d.ellipse([160, 72, 196, 108], fill=W)


def g_music(d):
    d.ellipse([40, 164, 104, 216], fill=W)
    d.ellipse([160, 140, 224, 192], fill=W)
    d.rectangle([88, 56, 104, 190], fill=W)
    d.rectangle([208, 32, 224, 166], fill=W)
    d.polygon([(88, 56), (224, 32), (224, 64), (88, 88)], fill=W)


def g_gear(d):
    cx, cy = 128, 128
    pts = []
    for i in range(32):
        a = i * math.pi * 2 / 32
        r = 112 if (i % 4) in (0, 1) else 84
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=W)
    d.ellipse([cx - 40, cy - 40, cx + 40, cy + 40], fill=(0, 0, 0, 0))


def g_user(d):
    d.ellipse([80, 28, 176, 124], fill=W)
    d.chord([28, 140, 228, 340], 180, 360, fill=W)


def g_lock(d):
    d.rounded_rectangle([44, 112, 212, 232], 18, fill=W)
    d.arc([76, 28, 180, 150], 180, 360, fill=W, width=LW)
    d.line([(76, 90), (76, 116)], fill=W, width=LW)
    d.line([(180, 90), (180, 116)], fill=W, width=LW)


def g_sleep(d):
    d.chord([28, 28, 228, 228], 60, 300, fill=W)
    d.ellipse([88, 16, 252, 180], fill=(0, 0, 0, 0))


def g_clock(d):
    d.ellipse([24, 24, 232, 232], outline=W, width=LW)
    d.line([(128, 128), (128, 64)], fill=W, width=LW - 2)
    d.line([(128, 128), (176, 156)], fill=W, width=LW - 2)


def g_keyboard(d):
    d.rounded_rectangle([16, 60, 240, 196], 20, outline=W, width=LW - 4)
    for y in (92, 124):
        for x in range(48, 216, 32):
            d.rectangle([x - 8, y - 8, x + 8, y + 8], fill=W)
    d.rectangle([80, 152, 176, 168], fill=W)


def g_mouse(d):
    d.rounded_rectangle([64, 20, 192, 236], 60, outline=W, width=LW - 4)
    d.line([(128, 24), (128, 100)], fill=W, width=LW - 6)
    d.line([(68, 100), (188, 100)], fill=W, width=LW - 6)


def g_info(d):
    d.ellipse([20, 20, 236, 236], outline=W, width=LW - 2)
    d.ellipse([114, 58, 142, 86], fill=W)
    d.rounded_rectangle([114, 106, 142, 196], 8, fill=W)


def g_taskbar(d):
    d.rounded_rectangle([16, 36, 240, 220], 16, outline=W, width=LW - 4)
    d.rectangle([16, 178, 240, 220], fill=W)


def g_palette(d):
    d.ellipse([20, 28, 236, 228], outline=W, width=LW - 4)
    for x, y in ((80, 88), (128, 64), (176, 88), (184, 140)):
        d.ellipse([x - 16, y - 16, x + 16, y + 16], fill=W)
    d.ellipse([88, 150, 132, 194], fill=W)


GLYPHS = {
    "power": g_power, "restart": g_restart, "search": g_search, "volume": g_volume,
    "volume-low": lambda d: g_volume(d, 1), "volume-mute": g_volume_mute, "wifi": g_network,
    "net": g_network_wired, "net-off": g_network_off, "back": g_back, "forward": g_forward, "up": g_up,
    "down": g_down, "chev-down": g_chev_down, "chev-up": g_chev_up, "chev-right": g_chev_right,
    "chev-left": g_chev_left, "home": g_home, "refresh": g_refresh, "folder": g_folder,
    "newfolder": g_newfolder, "file": g_file, "newfile": g_newfile, "delete": g_delete, "rename": g_rename,
    "save": g_save, "open": g_open, "copy": g_copy, "paste": g_paste, "cut": g_cut, "play": g_play,
    "pause": g_pause, "stop": g_stop, "next": g_next, "prev": g_prev, "close": g_close, "check": g_check,
    "plus": g_plus, "minus": g_minus, "menu": g_menu, "undo": g_undo, "redo": g_redo, "pencil": g_pencil,
    "brush": g_brush, "eraser": g_eraser, "line": g_line, "rect": g_rect, "ellipse": g_ellipse,
    "fill": g_fill, "picker": g_picker, "text": g_text, "zoom-in": g_zoom_in, "zoom-out": g_zoom_out,
    "flag": g_flag, "grid": g_grid, "list": g_list, "computer": g_computer, "image": g_image,
    "music": g_music, "gear": g_gear, "user": g_user, "lock": g_lock, "sleep": g_sleep,
    "clock": g_clock, "keyboard": g_keyboard, "mouse": g_mouse, "info": g_info, "taskbar": g_taskbar,
    "palette": g_palette,
}

# ----------------------------------------------------------------------------- cursors (32x32)
def cursor_img(draw_fn, size=32):
    big = 8
    img = Image.new("RGBA", (size * big, size * big), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(img), big)
    return img.resize((size, size), Image.LANCZOS)


def outlined_poly(d, pts, k, fill=(255, 255, 255, 255)):
    p = [(x * k, y * k) for x, y in pts]
    d.polygon(p, fill=(0, 0, 0, 255))
    # inset fill: shrink towards centroid
    cx = sum(x for x, _ in p) / len(p)
    cy = sum(y for _, y in p) / len(p)
    inner = []
    for x, y in p:
        dx, dy = cx - x, cy - y
        l = math.hypot(dx, dy) or 1
        inner.append((x + dx / l * k * 1.3, y + dy / l * k * 1.3))
    d.polygon(inner, fill=fill)


def c_arrow(d, k):
    pts = [(1, 1), (1, 20), (6, 15.5), (9.5, 23), (12.5, 21.5), (9.2, 14.5), (15.5, 14.5)]
    outlined_poly(d, pts, k)


def c_text(d, k):
    for w, col in ((5, (0, 0, 0, 255)), (2.2, (255, 255, 255, 255))):
        d.line([(16 * k, 5 * k), (16 * k, 27 * k)], fill=col, width=int(w * k))
        d.line([(11 * k, 4.5 * k), (21 * k, 4.5 * k)], fill=col, width=int(w * k))
        d.line([(11 * k, 27.5 * k), (21 * k, 27.5 * k)], fill=col, width=int(w * k))


def c_hand(d, k):
    pts = [(12, 2), (15, 2), (15, 12), (18, 11), (21, 12), (24, 13.5), (26, 16), (26, 23), (23, 29),
           (13, 29), (8, 22), (5, 17), (7, 15), (12, 19)]
    outlined_poly(d, pts, k)


def c_wait(d, k):
    d.ellipse([4 * k, 4 * k, 28 * k, 28 * k], fill=(0, 0, 0, 255))
    d.ellipse([6 * k, 6 * k, 26 * k, 26 * k], fill=(255, 255, 255, 255))
    d.pieslice([8 * k, 8 * k, 24 * k, 24 * k], -90, 60, fill=(217, 119, 87, 255))
    d.pieslice([8 * k, 8 * k, 24 * k, 24 * k], 90, 240, fill=(217, 119, 87, 255))


def double_arrow(d, k, angle):
    def rot(x, y):
        a = math.radians(angle)
        x, y = x - 16, y - 16
        return (16 + x * math.cos(a) - y * math.sin(a), 16 + x * math.sin(a) + y * math.cos(a))
    pts = [(2, 16), (9, 9), (9, 13.5), (23, 13.5), (23, 9), (30, 16), (23, 23), (23, 18.5), (9, 18.5), (9, 23)]
    outlined_poly(d, [rot(x, y) for x, y in pts], k)


def c_move(d, k):
    double_arrow(d, k, 0)
    double_arrow(d, k, 90)


def c_cross(d, k):
    for w, col in ((5, (0, 0, 0, 255)), (2, (255, 255, 255, 255))):
        d.line([(16 * k, 3 * k), (16 * k, 29 * k)], fill=col, width=int(w * k))
        d.line([(3 * k, 16 * k), (29 * k, 16 * k)], fill=col, width=int(w * k))


CURSORS = [("arrow", c_arrow), ("text", c_text), ("hand", c_hand), ("wait", c_wait),
           ("resize-h", lambda d, k: double_arrow(d, k, 0)), ("resize-v", lambda d, k: double_arrow(d, k, 90)),
           ("resize-nwse", lambda d, k: double_arrow(d, k, 45)), ("resize-nesw", lambda d, k: double_arrow(d, k, -45)),
           ("move", c_move), ("cross", c_cross)]


def main():
    os.makedirs(OUT, exist_ok=True)
    n = 0
    for name, fn in COLOR_ICONS.items():
        img = canvas()
        fn(img)
        img = shadow(img, (24, 24, 232, 232), 40, 60, 7, 5) if name not in ("logo",) else img
        finish(img, name)
        n += 1
    gdir = os.path.join(OUT, "glyph")
    os.makedirs(gdir, exist_ok=True)
    for name, fn in GLYPHS.items():
        img = canvas()
        fn(ImageDraw.Draw(img))
        for s in (16, 20, 24, 32):
            save_icn(img.resize((s, s), Image.LANCZOS), os.path.join(gdir, "%s-%d.icn" % (name, s)))
        n += 1
    cdir = os.path.join(OUT, "cursor")
    os.makedirs(cdir, exist_ok=True)
    for name, fn in CURSORS:
        save_icn(cursor_img(fn), os.path.join(cdir, name + ".icn"))
        n += 1
    # preview sheet for humans
    sheet = Image.new("RGBA", (48 * 13, 48 * 3 + 32 * 6), (40, 44, 52, 255))
    x = y = 0
    for i, name in enumerate(COLOR_ICONS):
        img = canvas()
        COLOR_ICONS[name](img)
        sheet.alpha_composite(img.resize((48, 48), Image.LANCZOS), ((i % 13) * 48, (i // 13) * 48))
    for i, name in enumerate(GLYPHS):
        img = canvas()
        GLYPHS[name](ImageDraw.Draw(img))
        sheet.alpha_composite(img.resize((24, 24), Image.LANCZOS), ((i % 24) * 26 + 2, 48 * 2 + 8 + (i // 24) * 30))
    sheet.save(os.path.join(os.path.dirname(OUT.rstrip("/")), "..", "build", "icons-preview.png")
               if os.path.isdir(os.path.join(os.path.dirname(__file__), "..", "build")) else "/dev/null", "PNG")
    print("generated %d icons" % n)


if __name__ == "__main__":
    main()
