#!/usr/bin/env python3
"""Pixel-art icons for the idle glance cards (16x16, drawn here, shown 3x on the glass).

    python3 tools/muse/glance_icons.py            # writes components/muse/glance_icons.h
    python3 tools/muse/glance_icons.py --preview out.png

Each icon is drawn with whole-pixel primitives (no anti-aliasing) from a small shared palette,
then stored as ARGB8888 for lv_image (scaled 3x without smoothing on the device).
"""
import math
import os
import sys

from PIL import Image

N = 16
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "components", "muse", "glance_icons.h")

C = {
    "Y": (255, 214, 64), "y": (255, 168, 40), "W": (236, 244, 248), "w": (170, 186, 196),
    "g": (110, 124, 134), "B": (92, 200, 255), "b": (50, 130, 220), "O": (255, 154, 92),
    "o": (196, 98, 52), "G": (77, 255, 60), "d": (40, 150, 40), "K": (52, 60, 66), "P": (199, 146, 255),
    "R": (255, 92, 92), "r": (170, 40, 40), "T": (77, 216, 200), "t": (30, 120, 112),
}


def canvas():
    return [[None] * N for _ in range(N)]


def put(c, x, y, k):
    if 0 <= x < N and 0 <= y < N:
        c[y][x] = k


def disc(c, cx, cy, r, k, edge=None):
    for y in range(N):
        for x in range(N):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            if d <= r:
                put(c, x, y, edge if edge and d > r - 1.0 else k)


def rect(c, x0, y0, x1, y1, k):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(c, x, y, k)


def cloud(c, dx=0, dy=0, body="W", shade="w"):
    for cx, cy, r in ((5.5, 9.5, 3.2), (9.0, 7.5, 4.0), (12.2, 10.0, 2.8)):
        disc(c, cx + dx, cy + dy, r, body)
    rect(c, 3 + dx, 10 + dy, 13 + dx, 12 + dy, body)
    for x in range(3 + dx, 14 + dx):          # a flat, shaded underside
        put(c, x, 12 + dy, shade)
    for y in range(N):
        for x in range(N):
            if c[y][x] == body and (y + 1 >= N or c[y + 1][x] is None) and y > 9 + dy:
                c[y][x] = shade


def sun(c, cx=7.5, cy=7.5, r=3.6, rays=True):
    disc(c, cx, cy, r, "Y", edge="y")
    if rays:
        for i in range(8):
            a = i * math.pi / 4
            for d in (r + 1.8, r + 2.8):
                put(c, int(cx + math.cos(a) * d), int(cy + math.sin(a) * d), "y" if d > r + 2 else "Y")


def icon_sun():
    c = canvas(); sun(c); return c


def icon_partly():
    c = canvas(); sun(c, 6.0, 5.5, 3.0); cloud(c, 1, 2); return c


def icon_cloud():
    c = canvas(); cloud(c, 0, 0); cloud(c, -2, -3, "w", "g") if False else None; return c


def icon_rain():
    c = canvas(); cloud(c, 0, -3, "w", "g")
    for x, y in ((4, 12), (8, 12), (12, 12), (6, 14), (10, 14)):
        put(c, x, y, "B"); put(c, x - 1, y + 1, "b")
    return c


def icon_snow():
    c = canvas(); cloud(c, 0, -3)
    for x, y in ((4, 12), (9, 12), (13, 13), (6, 14), (11, 15)):
        put(c, x, y, "W"); put(c, x - 1, y, "w"); put(c, x + 1, y, "w"); put(c, x, y - 1, "w"); put(c, x, y + 1, "w")
    return c


def icon_storm():
    c = canvas(); cloud(c, 0, -4, "g", "K")
    bolt = ((10, 8), (9, 9), (8, 10), (7, 11), (8, 11), (9, 11), (10, 11), (9, 12), (8, 13), (7, 14), (6, 15))
    for x, y in bolt:
        put(c, x, y, "Y"); put(c, x - 1, y, "y")
    return c


def icon_fog():
    c = canvas()
    for i, y in enumerate((5, 8, 11)):
        rect(c, 2 + (i % 2) * 2, y, 13 - ((i + 1) % 2) * 2, y, "w")
        put(c, 2 + (i % 2) * 2, y, "g"); put(c, 13 - ((i + 1) % 2) * 2, y, "g")
    return c


def icon_clock():
    c = canvas()
    disc(c, 8, 8, 7.0, "d")
    disc(c, 8, 8, 5.9, None) if False else None
    for y in range(N):
        for x in range(N):
            if math.hypot(x + 0.5 - 8, y + 0.5 - 8) < 6.0:
                c[y][x] = "K"
    for y in range(3, 8):
        put(c, 7, y, "G")                        # minute hand to 12
    for x in range(7, 11):
        put(c, x, 8, "G")                        # hour hand to 3
    put(c, 7, 8, "W")
    for a in range(12):
        t = a * math.pi / 6
        put(c, int(8 + math.cos(t) * 5.2), int(8 + math.sin(t) * 5.2), "g")
    return c


def icon_printer():
    c = canvas()
    rect(c, 1, 1, 14, 1, "w"); rect(c, 1, 1, 1, 14, "w"); rect(c, 14, 1, 14, 14, "w")   # frame
    rect(c, 2, 4, 13, 4, "g")                     # gantry
    rect(c, 6, 3, 9, 6, "K"); rect(c, 7, 7, 8, 7, "O")   # hotend + nozzle glow
    rect(c, 5, 10, 10, 12, "O"); rect(c, 6, 9, 9, 9, "o")  # the part being printed
    rect(c, 2, 13, 13, 13, "B")                   # bed
    rect(c, 1, 14, 14, 14, "w")
    return c


def icon_bell():
    c = canvas()
    disc(c, 8.0, 6.5, 4.6, "Y", edge="y")          # the dome
    for y in range(6, 12):                          # the skirt flares to the rim
        half = 4 + (y - 6) // 2 + (1 if y >= 10 else 0)
        rect(c, 8 - half, y, 7 + half, y, "Y")
        put(c, 8 - half, y, "y"); put(c, 7 + half, y, "y")
    rect(c, 1, 12, 14, 12, "y")
    rect(c, 7, 13, 8, 14, "O")                      # clapper
    put(c, 7, 1, "y"); put(c, 8, 1, "y")
    put(c, 5, 4, "W"); put(c, 5, 5, "W"); put(c, 4, 6, "W")   # a glint
    return c


def icon_spark():
    c = canvas()
    for i in range(-5, 6):
        put(c, 7 + i, 7, "G" if abs(i) < 3 else "d"); put(c, 7, 7 + i, "G" if abs(i) < 3 else "d")
    rect(c, 6, 6, 8, 8, "W")
    for x, y in ((12, 2), (2, 12), (13, 12)):
        put(c, x, y, "G"); put(c, x + 1, y, "d"); put(c, x - 1, y, "d"); put(c, x, y + 1, "d"); put(c, x, y - 1, "d")
    return c


def icon_note():
    c = canvas()
    rect(c, 3, 1, 12, 14, "W"); rect(c, 3, 14, 12, 14, "w"); rect(c, 12, 1, 12, 14, "w")
    for y in (4, 7, 10):
        rect(c, 5, y, 10, y, "g")
    return c


def icon_task():
    c = canvas()
    disc(c, 8, 8, 7.2, "P", edge="b")
    for x, y in ((4, 8), (5, 9), (6, 10), (7, 11), (8, 10), (9, 9), (10, 8), (11, 7), (12, 6)):
        put(c, x, y, "W"); put(c, x, y - 1, "W")
    return c


def icon_agent():
    """Hermes: a small robot head with an antenna and two lit eyes."""
    c = canvas()
    put(c, 7, 0, "P"); put(c, 8, 0, "P"); rect(c, 7, 1, 8, 2, "w")
    rect(c, 2, 3, 13, 12, "w"); rect(c, 3, 4, 12, 11, "K")
    rect(c, 1, 6, 1, 9, "g"); rect(c, 14, 6, 14, 9, "g")          # ears
    rect(c, 4, 6, 6, 8, "P"); rect(c, 9, 6, 11, 8, "P")          # eyes
    put(c, 4, 6, "W"); put(c, 9, 6, "W")
    rect(c, 5, 10, 10, 10, "g")                                   # mouth grille
    for x in (6, 8):
        put(c, x, 10, "K")
    rect(c, 4, 13, 11, 14, "g")                                   # neck/shoulders
    return c


def icon_chip():
    """This machine: a processor die with pins."""
    c = canvas()
    for i in range(3, 13, 3):
        rect(c, i, 0, i, 2, "w"); rect(c, i, 13, i, 15, "w")      # pins top/bottom
        rect(c, 0, i, 2, i, "w"); rect(c, 13, i, 15, i, "w")      # pins left/right
    rect(c, 2, 2, 13, 13, "t"); rect(c, 3, 3, 12, 12, "K")
    rect(c, 5, 5, 10, 10, "T"); rect(c, 6, 6, 9, 9, "t")
    put(c, 3, 3, "T")                                             # pin-1 mark
    return c


def icon_alert():
    """Something needs a look: a red warning triangle."""
    c = canvas()
    for y in range(2, 15):
        half = (y - 1) // 2
        rect(c, 8 - half - 1, y, 7 + half + 1, y, "R")
    rect(c, 7, 6, 8, 10, "W"); rect(c, 7, 12, 8, 13, "W")
    for y in range(2, 15):
        half = (y - 1) // 2
        put(c, 8 - half - 1, y, "r"); put(c, 7 + half + 1, y, "r")
    rect(c, 1, 14, 14, 14, "r")
    return c


ICONS = {
    "sun": icon_sun, "partly": icon_partly, "cloud": icon_cloud, "rain": icon_rain, "snow": icon_snow,
    "storm": icon_storm, "fog": icon_fog, "clock": icon_clock, "printer": icon_printer, "bell": icon_bell,
    "spark": icon_spark, "note": icon_note, "task": icon_task, "agent": icon_agent, "chip": icon_chip,
    "alert": icon_alert,
}


def rgba(c):
    return [(C[k] + (255,)) if k else (0, 0, 0, 0) for row in c for k in row]


def preview(path, scale=6):
    names = list(ICONS)
    im = Image.new("RGBA", (len(names) * (N * scale + 8), N * scale + 8), (8, 14, 12, 255))
    for i, n in enumerate(names):
        icon = Image.new("RGBA", (N, N)); icon.putdata(rgba(ICONS[n]()))
        im.alpha_composite(icon.resize((N * scale, N * scale), Image.NEAREST), (i * (N * scale + 8) + 4, 4))
    im.save(path)


def header():
    out = ["/* Generated by tools/muse/glance_icons.py -- do not edit. 16x16 ARGB8888 (B,G,R,A bytes). */",
           "#pragma once", "#include <stdint.h>", "#define GLANCE_ICON_N 16", ""]
    names = list(ICONS)
    for n in names:
        px = rgba(ICONS[n]())
        data = ", ".join("0x%02x,0x%02x,0x%02x,0x%02x" % (b, g, r, a) for r, g, b, a in px)
        out.append("static const uint8_t GLANCE_ICON_%s[%d] = { %s };" % (n.upper(), N * N * 4, data))
    out.append("")
    out.append("static const struct { const char *name; const uint8_t *px; } GLANCE_ICONS[] = {")
    out += ['    { "%s", GLANCE_ICON_%s },' % (n, n.upper()) for n in names]
    out.append("};")
    open(OUT, "w").write("\n".join(out) + "\n")
    print("wrote", os.path.relpath(OUT, ROOT), "(%d icons)" % len(names))


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--preview":
        preview(sys.argv[2])
    else:
        header()
