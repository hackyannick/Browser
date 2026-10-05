#!/usr/bin/env python3
"""Generates src/win32/kite.ico (a stylised kite) with Pillow."""
from PIL import Image, ImageDraw

def draw(size):
    s = size * 4
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    top, right, bottom, left = (s * 0.52, s * 0.04), (s * 0.90, s * 0.40), (s * 0.40, s * 0.86), (s * 0.12, s * 0.30)
    center = (s * 0.49, s * 0.38)
    d.polygon([top, right, center], fill=(231, 76, 60, 255))
    d.polygon([right, bottom, center], fill=(241, 196, 15, 255))
    d.polygon([bottom, left, center], fill=(46, 134, 222, 255))
    d.polygon([left, top, center], fill=(39, 174, 96, 255))
    d.line([top, bottom], fill=(40, 40, 60, 255), width=max(1, s // 40))
    d.line([left, right], fill=(40, 40, 60, 255), width=max(1, s // 40))
    # Tail with bows.
    d.line([bottom, (s * 0.30, s * 0.97)], fill=(60, 60, 80, 255), width=max(1, s // 32))
    for t in (0.35, 0.7):
        x = bottom[0] + (s * 0.30 - bottom[0]) * t
        y = bottom[1] + (s * 0.97 - bottom[1]) * t
        d.polygon([(x - s * 0.06, y - s * 0.03), (x, y), (x - s * 0.06, y + s * 0.03)], fill=(231, 76, 60, 255))
        d.polygon([(x + s * 0.06, y - s * 0.03), (x, y), (x + s * 0.06, y + s * 0.03)], fill=(231, 76, 60, 255))
    return img.resize((size, size), Image.LANCZOS)

imgs = [draw(n) for n in (48, 32, 16)]
imgs[0].save("src/win32/kite.ico", sizes=[(48, 48), (32, 32), (16, 16)], append_images=imgs[1:])
imgs[0].save("docs/kite-icon.png")
