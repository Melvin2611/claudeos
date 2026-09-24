#!/usr/bin/env python3
"""Generate sample pictures (24-bit BMP) for the ClaudeOS home folder."""
import math, os, random, sys
from PIL import Image, ImageDraw, ImageFilter

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "rootfs", "home", "Pictures")
os.makedirs(OUT, exist_ok=True)


def mandelbrot(w=800, h=600):
    img = Image.new("RGB", (w, h))
    px = img.load()
    maxit = 120
    for y in range(h):
        for x in range(w):
            cr = -2.2 + 3.1 * x / w
            ci = -1.2 + 2.4 * y / h
            zr = zi = 0.0
            i = 0
            while zr * zr + zi * zi < 4 and i < maxit:
                zr, zi = zr * zr - zi * zi + cr, 2 * zr * zi + ci
                i += 1
            if i == maxit:
                px[x, y] = (10, 8, 20)
            else:
                t = i + 1 - math.log(math.log(max(1.0001, math.sqrt(zr * zr + zi * zi)))) / math.log(2)
                t = t / maxit
                r = int(255 * min(1, 9 * (1 - t) * t * t * t * 4))
                g = int(255 * min(1, 15 * (1 - t) * (1 - t) * t * t * 2))
                b = int(255 * min(1, 8.5 * (1 - t) ** 3 * t * 3))
                px[x, y] = (min(255, r + 40), min(255, g + 20), min(255, b + 60))
    return img


def sunset(w=800, h=600):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    top, mid, low = (38, 20, 71), (217, 119, 87), (255, 206, 140)
    for y in range(h):
        t = y / (h * 0.62)
        if t < 1:
            c = tuple(int(top[i] + (mid[i] - top[i]) * t) for i in range(3)) if t < 0.7 else \
                tuple(int(mid[i] + (low[i] - mid[i]) * (t - 0.7) / 0.3) for i in range(3))
        else:
            c = low
        d.line([(0, y), (w, y)], fill=c)
    d.ellipse([w * 0.58, h * 0.40, w * 0.72, h * 0.40 + w * 0.14], fill=(255, 236, 200))
    random.seed(4)
    for layer, col in enumerate([(120, 60, 80), (80, 38, 64), (48, 24, 48), (24, 14, 30)]):
        base = h * (0.55 + layer * 0.1)
        pts = [(0, h)]
        x = 0
        yv = base
        while x <= w:
            yv += random.uniform(-18, 18) * (1 + layer * 0.3)
            yv = max(base - 90, min(base + 30, yv))
            pts.append((x, yv))
            x += 16
        pts.append((w, h))
        d.polygon(pts, fill=col)
    return img.filter(ImageFilter.SMOOTH)


def geometric(w=800, h=600):
    img = Image.new("RGB", (w, h), (24, 26, 34))
    d = ImageDraw.Draw(img)
    random.seed(7)
    palette = [(217, 119, 87), (91, 155, 255), (74, 222, 128), (245, 166, 35), (142, 78, 198), (232, 72, 85)]
    for i in range(60):
        x, y = random.randint(-50, w), random.randint(-50, h)
        s = random.randint(30, 160)
        c = random.choice(palette)
        shape = random.randint(0, 2)
        if shape == 0:
            d.ellipse([x, y, x + s, y + s], outline=c, width=4)
        elif shape == 1:
            d.rectangle([x, y, x + s, y + s], outline=c, width=4)
        else:
            d.polygon([(x, y + s), (x + s / 2, y), (x + s, y + s)], outline=c, width=4)
    return img


for name, fn in [("Mandelbrot.bmp", mandelbrot), ("Sunset.bmp", sunset), ("Shapes.bmp", geometric)]:
    path = os.path.join(OUT, name)
    fn().save(path, "BMP")
    print("wrote", path, os.path.getsize(path))
