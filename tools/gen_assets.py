#!/usr/bin/env python3
"""Generate embedded assets for P4Box firmware.

Outputs src/app/assets/assets_gen.c with:
  * TTF font blobs (rendered at runtime by LVGL tiny_ttf)
  * App icons as ARGB8888 lv_image_dsc_t (drawn here with Pillow)

Run:  python3 tools/gen_assets.py
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "src", "app", "assets")
PREVIEW_DIR = os.path.join(ROOT, "tools", "preview")
FONT_DIR = "/usr/share/fonts/truetype/google-fonts"

FONTS = {
    "poppins_light": "Poppins-Light.ttf",
    "poppins_regular": "Poppins-Regular.ttf",
    "poppins_medium": "Poppins-Medium.ttf",
    "poppins_bold": "Poppins-Bold.ttf",
}

ICON = 112          # final icon size in px
SS = 4              # supersampling factor
S = ICON * SS       # drawing canvas size


def font(name, px):
    return ImageFont.truetype(os.path.join(FONT_DIR, FONTS[name]), px)


def rounded_bg(top, bottom):
    """Rounded-square app tile with a vertical gradient."""
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / (S - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (S, y)], fill=c)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.23), fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    # subtle top highlight
    hl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(hl).rounded_rectangle([0, 0, S - 1, S // 2], radius=int(S * 0.23), fill=(255, 255, 255, 22))
    hmask = Image.new("L", (S, S), 0)
    hmask.paste(mask, (0, 0))
    out = Image.alpha_composite(out, Image.composite(hl, Image.new("RGBA", (S, S)), hmask))
    return out


def shadowed(layer, blur=10, offset=6, alpha=90):
    """Drop shadow under a white glyph layer."""
    a = layer.split()[3]
    sh = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    sh.putalpha(a.point(lambda v: v * alpha // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    base = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    base.paste(sh, (0, offset), sh)
    return Image.alpha_composite(base, layer)


def icon_clock():
    bg = rounded_bg((64, 156, 255), (28, 92, 214))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    c = S // 2
    r = int(S * 0.33)
    d.ellipse([c - r, c - r, c + r, c + r], fill=(255, 255, 255, 255))
    for i in range(12):
        a = math.radians(i * 30)
        r1 = r * (0.78 if i % 3 else 0.70)
        r2 = r * 0.88
        w = int(S * (0.022 if i % 3 else 0.03))
        d.line([(c + r1 * math.sin(a), c - r1 * math.cos(a)), (c + r2 * math.sin(a), c - r2 * math.cos(a))],
               fill=(40, 70, 130, 255), width=w)
    # hands at 10:10
    def hand(angle_deg, length, width, col):
        a = math.radians(angle_deg)
        d.line([(c, c), (c + length * math.sin(a), c - length * math.cos(a))], fill=col, width=width)
        d.ellipse([c - width / 2, c - width / 2, c + width / 2, c + width / 2], fill=col)
        ex, ey = c + length * math.sin(a), c - length * math.cos(a)
        d.ellipse([ex - width / 2, ey - width / 2, ex + width / 2, ey + width / 2], fill=col)
    hand(-60, r * 0.5, int(S * 0.045), (25, 40, 70, 255))
    hand(60, r * 0.72, int(S * 0.034), (25, 40, 70, 255))
    hand(170, r * 0.78, int(S * 0.014), (240, 90, 60, 255))
    cr = int(S * 0.03)
    d.ellipse([c - cr, c - cr, c + cr, c + cr], fill=(240, 90, 60, 255))
    return Image.alpha_composite(bg, shadowed(g))


def icon_radio():
    bg = rounded_bg((255, 160, 60), (232, 96, 28))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    x0, y0, x1, y1 = int(S * 0.18), int(S * 0.38), int(S * 0.82), int(S * 0.78)
    # antenna
    d.line([(int(S * 0.34), y0), (int(S * 0.66), int(S * 0.20))], fill=W, width=int(S * 0.03))
    d.ellipse([int(S * 0.63), int(S * 0.17), int(S * 0.69), int(S * 0.23)], fill=W)
    d.rounded_rectangle([x0, y0, x1, y1], radius=int(S * 0.06), fill=W)
    # speaker
    sc = (int(S * 0.36), int(S * 0.58))
    for rr, col in ((0.12, (232, 110, 40, 255)), (0.075, W), (0.035, (232, 110, 40, 255))):
        r = int(S * rr)
        d.ellipse([sc[0] - r, sc[1] - r, sc[0] + r, sc[1] + r], fill=col)
    # dial + buttons
    d.rounded_rectangle([int(S * 0.56), int(S * 0.46), int(S * 0.76), int(S * 0.54)], radius=int(S * 0.02),
                        fill=(232, 110, 40, 255))
    d.line([(int(S * 0.62), int(S * 0.46)), (int(S * 0.62), int(S * 0.54))], fill=W, width=int(S * 0.012))
    for i in range(3):
        bx = int(S * (0.58 + i * 0.07))
        d.ellipse([bx - int(S * 0.022), int(S * 0.64) - int(S * 0.022), bx + int(S * 0.022), int(S * 0.64) + int(S * 0.022)],
                  fill=(232, 110, 40, 255))
    return Image.alpha_composite(bg, shadowed(g))


def icon_lock():
    bg = rounded_bg((72, 200, 120), (24, 140, 76))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    # shackle
    sw = int(S * 0.07)
    d.arc([int(S * 0.32), int(S * 0.16), int(S * 0.68), int(S * 0.56)], 180, 360, fill=W, width=sw)
    d.line([(int(S * 0.32) + sw // 2, int(S * 0.36)), (int(S * 0.32) + sw // 2, int(S * 0.46))], fill=W, width=sw)
    d.line([(int(S * 0.68) - sw // 2, int(S * 0.36)), (int(S * 0.68) - sw // 2, int(S * 0.46))], fill=W, width=sw)
    d.rounded_rectangle([int(S * 0.24), int(S * 0.44), int(S * 0.76), int(S * 0.82)], radius=int(S * 0.07), fill=W)
    # keyhole
    kc = (S // 2, int(S * 0.60))
    kr = int(S * 0.05)
    col = (30, 150, 84, 255)
    d.ellipse([kc[0] - kr, kc[1] - kr, kc[0] + kr, kc[1] + kr], fill=col)
    d.polygon([(kc[0] - int(S * 0.025), kc[1]), (kc[0] + int(S * 0.025), kc[1]),
               (kc[0] + int(S * 0.035), int(S * 0.72)), (kc[0] - int(S * 0.035), int(S * 0.72))], fill=col)
    return Image.alpha_composite(bg, shadowed(g))


def icon_calendar():
    bg = rounded_bg((255, 255, 255), (232, 236, 242))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    red = (230, 62, 62, 255)
    # header band
    hdr = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hdr)
    hd.rectangle([0, 0, S, int(S * 0.30)], fill=red)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.23), fill=255)
    bg = Image.alpha_composite(bg, Image.composite(hdr, Image.new("RGBA", (S, S)), mask))
    f = font("poppins_bold", int(S * 0.16))
    d.text((S // 2, int(S * 0.155)), "OCT", font=f, fill=(255, 255, 255, 255), anchor="mm")
    # grid of days
    dark = (60, 66, 80, 255)
    for row in range(4):
        for col in range(5):
            x = int(S * (0.20 + col * 0.15))
            y = int(S * (0.44 + row * 0.13))
            r = int(S * 0.032)
            fill = red if (row == 2 and col == 3) else dark
            if row == 2 and col == 3:
                d.ellipse([x - r * 2, y - r * 2, x + r * 2, y + r * 2], fill=red)
                d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, 255))
            else:
                d.rounded_rectangle([x - r, y - r, x + r, y + r], radius=r // 2, fill=fill)
    return Image.alpha_composite(bg, g)


def icon_speed():
    bg = rounded_bg((150, 100, 255), (92, 52, 214))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    c = (S // 2, int(S * 0.60))
    r = int(S * 0.34)
    aw = int(S * 0.075)
    d.arc([c[0] - r, c[1] - r, c[0] + r, c[1] + r], 150, 390, fill=(255, 255, 255, 110), width=aw)
    d.arc([c[0] - r, c[1] - r, c[0] + r, c[1] + r], 150, 300, fill=W, width=aw)
    # ticks
    for i in range(9):
        a = math.radians(150 + i * 30)
        r1, r2 = r * 0.62, r * 0.72
        d.line([(c[0] + r1 * math.cos(a), c[1] + r1 * math.sin(a)), (c[0] + r2 * math.cos(a), c[1] + r2 * math.sin(a))],
               fill=W, width=int(S * 0.018))
    a = math.radians(300)
    L = r * 0.62
    d.line([c, (c[0] + L * math.cos(a), c[1] + L * math.sin(a))], fill=(255, 210, 80, 255), width=int(S * 0.04))
    cr = int(S * 0.055)
    d.ellipse([c[0] - cr, c[1] - cr, c[0] + cr, c[1] + cr], fill=W)
    return Image.alpha_composite(bg, shadowed(g))


def icon_photos():
    bg = rounded_bg((255, 96, 150), (196, 40, 110))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    x0, y0, x1, y1 = int(S * 0.15), int(S * 0.22), int(S * 0.85), int(S * 0.78)
    d.rounded_rectangle([x0, y0, x1, y1], radius=int(S * 0.07), fill=W)
    # picture inside the frame
    m = int(S * 0.05)
    ix0, iy0, ix1, iy1 = x0 + m, y0 + m, x1 - m, y1 - m
    pic = Image.new("RGBA", (ix1 - ix0, iy1 - iy0), (255, 214, 226, 255))
    pd = ImageDraw.Draw(pic)
    pw, ph = pic.size
    pd.ellipse([int(pw * 0.66), int(ph * 0.12), int(pw * 0.86), int(ph * 0.12) + int(pw * 0.20)], fill=(255, 176, 46, 255))
    pd.polygon([(0, ph), (int(pw * 0.36), int(ph * 0.34)), (int(pw * 0.66), ph)], fill=(196, 40, 110, 255))
    pd.polygon([(int(pw * 0.40), ph), (int(pw * 0.68), int(ph * 0.52)), (pw, int(ph * 0.86)), (pw, ph)], fill=(228, 70, 136, 255))
    mask = Image.new("L", pic.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, pw - 1, ph - 1], radius=int(S * 0.035), fill=255)
    g.paste(pic, (ix0, iy0), mask)
    return Image.alpha_composite(bg, shadowed(g))


def icon_settings():
    bg = rounded_bg((140, 150, 166), (88, 98, 114))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    c = S // 2
    teeth = 8
    ro, ri = S * 0.34, S * 0.26
    pts = []
    for i in range(teeth * 4):
        a = 2 * math.pi * i / (teeth * 4)
        rr = ro if (i % 4) in (1, 2) else ri
        pts.append((c + rr * math.cos(a), c + rr * math.sin(a)))
    d.polygon(pts, fill=W)
    rr = int(S * 0.27)
    d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=W)
    rh = int(S * 0.11)
    d.ellipse([c - rh, c - rh, c + rh, c + rh], fill=(100, 110, 126, 255))
    return Image.alpha_composite(bg, shadowed(g))


def small_ethernet(size=36):
    s = size * SS
    g = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    # RJ45 plug outline
    d.rounded_rectangle([int(s * 0.18), int(s * 0.14), int(s * 0.82), int(s * 0.72)], radius=int(s * 0.08),
                        outline=W, width=int(s * 0.09))
    d.rectangle([int(s * 0.38), int(s * 0.72), int(s * 0.62), int(s * 0.90)], fill=W)
    for i in range(4):
        x = int(s * (0.32 + i * 0.12))
        d.line([(x, int(s * 0.28)), (x, int(s * 0.44))], fill=W, width=int(s * 0.05))
    return g.resize((size, size), Image.LANCZOS)


def small_padlock(size=22):
    s = size * SS
    g = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    sw = int(s * 0.12)
    d.arc([int(s * 0.26), int(s * 0.06), int(s * 0.74), int(s * 0.62)], 180, 360, fill=W, width=sw)
    d.line([(int(s * 0.26) + sw // 2, int(s * 0.34)), (int(s * 0.26) + sw // 2, int(s * 0.48))], fill=W, width=sw)
    d.line([(int(s * 0.74) - sw // 2, int(s * 0.34)), (int(s * 0.74) - sw // 2, int(s * 0.48))], fill=W, width=sw)
    d.rounded_rectangle([int(s * 0.14), int(s * 0.44), int(s * 0.86), int(s * 0.96)], radius=int(s * 0.1), fill=W)
    return g.resize((size, size), Image.LANCZOS)


def big_padlock(open_=False, size=120):
    s = size * SS
    g = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    W = (255, 255, 255, 255)
    sw = int(s * 0.10)
    x0, x1 = int(s * 0.27), int(s * 0.73)
    lift = int(s * 0.07) if open_ else 0
    shift = int(s * 0.15) if open_ else 0
    d.arc([x0 + shift, int(s * 0.09) - lift, x1 + shift, int(s * 0.59) - lift], 180, 360, fill=W, width=sw)
    d.line([(x1 + shift - sw // 2, int(s * 0.31) - lift), (x1 + shift - sw // 2, int(s * 0.47))], fill=W, width=sw)
    if not open_:
        d.line([(x0 + sw // 2, int(s * 0.31)), (x0 + sw // 2, int(s * 0.47))], fill=W, width=sw)
    d.rounded_rectangle([int(s * 0.16), int(s * 0.44), int(s * 0.84), int(s * 0.94)], radius=int(s * 0.09), fill=W)
    kc = (s // 2, int(s * 0.64))
    kr = int(s * 0.06)
    d.ellipse([kc[0] - kr, kc[1] - kr, kc[0] + kr, kc[1] + kr], fill=(0, 0, 0, 0))
    d.polygon([(kc[0] - int(s * 0.03), kc[1]), (kc[0] + int(s * 0.03), kc[1]), (kc[0] + int(s * 0.04), int(s * 0.80)),
               (kc[0] - int(s * 0.04), int(s * 0.80))], fill=(0, 0, 0, 0))
    return g.resize((size, size), Image.LANCZOS)


# ---------------------------------------------------------------- weather condition icons
SUN = (255, 196, 56, 255)
SUN_CORE = (255, 222, 90, 255)
MOON = (222, 230, 255, 255)
CLOUD = (238, 242, 248, 255)
CLOUD_DARK = (150, 162, 180, 255)
CLOUD_SHADE = (205, 213, 225, 255)
RAIN = (86, 170, 255, 255)
BOLT = (255, 206, 64, 255)


def _sun(d, cx, cy, r, s):
    for i in range(8):
        a = math.radians(i * 45 + 22.5)
        r1, r2 = r * 1.28, r * 1.62
        d.line([(cx + r1 * math.cos(a), cy + r1 * math.sin(a)), (cx + r2 * math.cos(a), cy + r2 * math.sin(a))],
               fill=SUN, width=int(s * 0.045))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=SUN)
    rr = r * 0.78
    d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=SUN_CORE)


def _moon(img, cx, cy, r):
    m = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(m)
    md.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    md.ellipse([cx - r * 0.35, cy - r * 1.15, cx + r * 1.45, cy + r * 0.65], fill=0)
    layer = Image.new("RGBA", img.size, MOON)
    img.paste(layer, (0, 0), m)


def _cloud(img, cx, cy, w, col, shade=None):
    """Cloud centred at (cx, cy), w wide; drawn as a mask so overlaps stay clean."""
    m = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(m)
    h = w * 0.36
    base_y = cy + h * 0.5
    md.rounded_rectangle([cx - w / 2, cy - h * 0.15, cx + w / 2, base_y], radius=h * 0.55, fill=255)
    for ox, oy, rr in ((-0.22, 0.02, 0.20), (0.04, -0.12, 0.27), (0.27, 0.06, 0.17)):
        x, y, r = cx + ox * w, cy + oy * w, rr * w
        md.ellipse([x - r, y - r, x + r, y + r], fill=255)
    if shade:
        sh = Image.new("RGBA", img.size, shade)
        img.paste(sh, (int(w * 0.02), int(w * 0.035)), m)
    img.paste(Image.new("RGBA", img.size, col), (0, 0), m)


def _drops(d, cx, top, w, s, n=3, heavy=False):
    for i in range(n):
        x = cx - w * 0.28 + i * (w * 0.56 / max(1, n - 1))
        L = s * (0.13 if heavy else 0.10)
        for k in range(2 if heavy else 1):
            y = top + k * s * 0.12 + (i % 2) * s * 0.05
            d.line([(x + L * 0.25, y), (x - L * 0.25, y + L)], fill=RAIN, width=int(s * 0.045))


def _flakes(d, cx, top, w, s):
    for i, (ox, oy) in enumerate(((-0.26, 0.02), (0.0, 0.10), (0.26, 0.02))):
        x, y, r = cx + ox * w, top + oy * s + s * 0.06, s * 0.055
        for a in (0, 60, 120):
            ar = math.radians(a)
            d.line([(x - r * math.cos(ar), y - r * math.sin(ar)), (x + r * math.cos(ar), y + r * math.sin(ar))],
                   fill=(255, 255, 255, 255), width=int(s * 0.026))


def _bolt(d, cx, top, s):
    pts = [(cx + s * 0.03, top), (cx - s * 0.10, top + s * 0.17), (cx - s * 0.005, top + s * 0.17),
           (cx - s * 0.06, top + s * 0.31), (cx + s * 0.11, top + s * 0.11), (cx + s * 0.015, top + s * 0.11),
           (cx + s * 0.07, top)]
    d.polygon(pts, fill=BOLT)


def weather_icon(code, size):
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    day = code.endswith("d")
    base = code[:2]
    if base == "01":
        if day: _sun(d, s / 2, s / 2, s * 0.22, s)
        else: _moon(img, s / 2, s / 2, s * 0.30)
    elif base == "02":
        if day: _sun(d, s * 0.40, s * 0.38, s * 0.16, s)
        else: _moon(img, s * 0.42, s * 0.36, s * 0.22)
        _cloud(img, s * 0.56, s * 0.60, s * 0.58, CLOUD, CLOUD_SHADE)
    elif base == "03":
        _cloud(img, s * 0.50, s * 0.54, s * 0.78, CLOUD, CLOUD_SHADE)
    elif base == "04":
        _cloud(img, s * 0.38, s * 0.42, s * 0.56, CLOUD_DARK)
        _cloud(img, s * 0.56, s * 0.60, s * 0.70, CLOUD, CLOUD_SHADE)
    elif base in ("09", "10"):
        if base == "10":
            if day: _sun(d, s * 0.33, s * 0.27, s * 0.14, s)
            else: _moon(img, s * 0.34, s * 0.26, s * 0.20)
        cx = s * 0.56 if base == "10" else s * 0.50
        _cloud(img, cx, s * 0.48, s * 0.68 if base == "10" else s * 0.76, CLOUD if base == "10" else CLOUD_DARK,
               CLOUD_SHADE if base == "10" else None)
        d = ImageDraw.Draw(img)
        _drops(d, cx, s * 0.72, s * 0.66, s, n=5 if base == "09" else 3)
    elif base == "11":
        _cloud(img, s * 0.50, s * 0.42, s * 0.78, CLOUD_DARK)
        d = ImageDraw.Draw(img)
        _bolt(d, s * 0.50, s * 0.60, s)
        _drops(d, s * 0.50, s * 0.66, s * 0.70, s, n=2)
    elif base == "13":
        _cloud(img, s * 0.50, s * 0.44, s * 0.76, CLOUD, CLOUD_SHADE)
        d = ImageDraw.Draw(img)
        _flakes(d, s * 0.50, s * 0.66, s * 0.76, s)
    else:  # 50 mist
        for i, (x0, x1) in enumerate(((0.18, 0.82), (0.26, 0.74), (0.14, 0.70), (0.30, 0.86))):
            y = s * (0.32 + i * 0.12)
            d.line([(s * x0, y), (s * x1, y)], fill=(200, 208, 220, 255), width=int(s * 0.06))
            r = s * 0.03
            for x in (s * x0, s * x1):
                d.ellipse([x - r, y - r, x + r, y + r], fill=(200, 208, 220, 255))
    return img.resize((size, size), Image.LANCZOS)


WX_CODES = ["01d", "01n", "02d", "02n", "03", "04", "09", "10d", "10n", "11", "13", "50"]


def icon_weather():
    bg = rounded_bg((92, 188, 255), (34, 116, 226))
    g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    _sun(d, S * 0.62, S * 0.36, S * 0.15, S)
    _cloud(g, S * 0.46, S * 0.62, S * 0.66, (255, 255, 255, 255), (210, 226, 245, 255))
    return Image.alpha_composite(bg, shadowed(g))


ICONS = {
    "icon_clock": icon_clock,
    "icon_weather": icon_weather,
    "icon_radio": icon_radio,
    "icon_lock": icon_lock,
    "icon_calendar": icon_calendar,
    "icon_speed": icon_speed,
    "icon_photos": icon_photos,
    "icon_settings": icon_settings,
}


def img_to_c(name, img):
    img = img.convert("RGBA")
    w, h = img.size
    px = img.tobytes()
    out = bytearray()
    for i in range(0, len(px), 4):
        r, g, b, a = px[i:i + 4]
        out += bytes((b, g, r, a))   # LVGL ARGB8888 memory order: B G R A
    lines = [f"static const uint8_t {name}_map[] = {{"]
    for i in range(0, len(out), 24):
        lines.append("  " + ",".join(f"0x{v:02x}" for v in out[i:i + 24]) + ",")
    lines.append("};")
    lines.append(f"const lv_image_dsc_t {name} = {{")
    lines.append("  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .flags = 0,")
    lines.append(f"              .w = {w}, .h = {h}, .stride = {w * 4} }},")
    lines.append(f"  .data_size = sizeof({name}_map),")
    lines.append(f"  .data = {name}_map,")
    lines.append("};\n")
    return "\n".join(lines)


def blob_to_c(name, data):
    lines = [f"const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 32):
        lines.append("  " + ",".join(f"0x{v:02x}" for v in data[i:i + 32]) + ",")
    lines.append("};")
    lines.append(f"const size_t {name}_len = {len(data)};\n")
    return "\n".join(lines)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    parts = ["/* Generated by tools/gen_assets.py - do not edit */",
             "#include \"assets.h\"\n",
             "/* Poppins font family: SIL Open Font License 1.1 (Indian Type Foundry) */"]
    for key, fname in FONTS.items():
        with open(os.path.join(FONT_DIR, fname), "rb") as f:
            parts.append(blob_to_c(f"ttf_{key}", f.read()))
    sheet = Image.new("RGBA", (ICON * len(ICONS) + 16 * (len(ICONS) + 1), ICON + 32), (14, 17, 22, 255))
    for i, (name, fn) in enumerate(ICONS.items()):
        img = fn().resize((ICON, ICON), Image.LANCZOS)
        parts.append(img_to_c(name, img))
        sheet.alpha_composite(img, (16 + i * (ICON + 16), 16))
    eth = small_ethernet()
    parts.append(img_to_c("icon_ethernet", eth))
    parts.append(img_to_c("icon_padlock_small", small_padlock()))
    parts.append(img_to_c("img_padlock_closed", big_padlock(False)))
    parts.append(img_to_c("img_padlock_open", big_padlock(True)))
    big_padlock(False).save(os.path.join(PREVIEW_DIR, "padlock_closed.png"))
    big_padlock(True).save(os.path.join(PREVIEW_DIR, "padlock_open.png"))
    sheet.save(os.path.join(PREVIEW_DIR, "icons.png"))
    wsheet = Image.new("RGBA", (len(WX_CODES) * 160 + 16, 160 + 90), (22, 27, 34, 255))
    for i, code in enumerate(WX_CODES):
        big = weather_icon(code, 150)
        small = weather_icon(code, 60)
        parts.append(img_to_c(f"wx_{code}_l", big))
        parts.append(img_to_c(f"wx_{code}_s", small))
        wsheet.alpha_composite(big, (8 + i * 160, 8))
        wsheet.alpha_composite(small, (8 + i * 160 + 45, 170))
    wsheet.save(os.path.join(PREVIEW_DIR, "weather_icons.png"))
    with open(os.path.join(OUT_DIR, "assets_gen.c"), "w") as f:
        f.write("\n".join(parts))
    print("wrote", os.path.join(OUT_DIR, "assets_gen.c"))


if __name__ == "__main__":
    sys.exit(main())
