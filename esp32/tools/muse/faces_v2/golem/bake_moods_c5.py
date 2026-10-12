#!/usr/bin/env python3
"""bake_moods.py: bake all 52 relay reactions as sparse-diff clips for a face.

Usage (from the muse-gadget-sdk esp32 root):
    /opt/homebrew/bin/python3 ~/muse/art-pipeline/moods_v2/bake_moods.py golem components/muse/avatar/golem_moods.h
    /opt/homebrew/bin/python3 ~/muse/art-pipeline/moods_v2/bake_moods.py stone components/muse/avatar/stone_moods.h

Format matches the golem's original golem_moods.h (MOODS[], MOOD_FRAME_COUNTS[],
MOOD_FRAME_PX[], MOOD_FRAME_C[]). Colours are ABSOLUTE indices in the renderer's
dynamic palette, so each clip rides the current mode's colour scheme.

The C player paints a diff only over dark or gold under-pixels and never over the
eye lenses (see muse_pixel_golem.c, the baked-mood block). Props are written to work
with that rule: sky first, then the body's gold and dark areas.

TWO HARD PLACEMENT RULES (learned from the 2026-10-11 full-animation audit):
  1. The vignette clips every prop pixel at distance > 29.0 from (31.5, 31.5).
     Anything drawn nearer a corner than ~27.5 loses its ends silently and the
     fragments read as STRAY PIXELS. Old offenders: shrug's arms (wholly outside),
     embarrassed's spiral, scared's side strokes, nod's chevrons, invoking's wand,
     sunny's outer rays, surprised's "!" glyphs, reading's text lines.
  2. At 64 px a 1 px white mark reads as dust. Props that must read use 2 px
     strokes, 2x2 bits, dense rings (dot_ring step 12 or finer), or 5x7 glyphs.
"""
import math
import os
import re
import sys

import numpy as np

TAU = 2.0 * math.pi
FRAME_MS = 167
HERE = os.path.dirname(os.path.abspath(__file__))
AVATAR = os.path.normpath(os.path.join(HERE, ".."))   # faces_v2/stage_b/golem: sprite_golem.h lives here (stage B pack)

# durations match REACTIONS[] in the firmware (seconds); the C side loops each clip over its duration
DURS = {
    "smirk": 2.8, "scan": 2.6, "alert": 2.2, "glitch": 1.8, "squint": 2.6, "angry": 2.6,
    "surprised": 1.8, "pleased": 2.6, "laugh": 2.4, "wink": 1.6, "sleepy": 3.4, "confused": 2.6,
    "evil": 3.0, "hacked": 2.4, "powerup": 2.4, "bored": 3.2, "disgust": 2.4, "target": 2.6,
    "nervous": 2.6, "proud": 2.8, "excited": 2.8, "confetti": 3.4, "sunny": 3.0, "rain": 3.2,
    "storm": 3.2, "cold": 3.0, "rainbow": 3.2, "searching": 2.6, "browsing": 3.0, "reading": 3.0,
    "writing": 3.0, "checking": 2.4, "calculating": 3.0, "calendar": 3.0, "monitoring": 2.8,
    "generating": 3.0, "running": 3.0, "invoking": 2.8, "delegating": 2.8, "remembering": 2.8,
    "shrug": 2.6, "embarrassed": 2.8, "scared": 2.6, "cool": 2.8, "mind_blown": 2.8,
    "crying_laughing": 2.8, "sad": 3.0, "nod": 2.4, "cloudy": 3.0, "windy": 3.0, "hot": 3.0,
    "snow": 3.4,
}

# 3x5 glyphs: each row is 3 bits, MSB = left column
FONT = {
    "0": [7, 5, 5, 5, 7], "1": [2, 6, 2, 2, 7], "?": [7, 1, 3, 0, 2], "!": [2, 2, 2, 0, 2],
    "Z": [7, 1, 2, 4, 7], "+": [0, 2, 7, 2, 0], "=": [0, 7, 0, 7, 0], ".": [0, 0, 0, 0, 2],
}

# 5x7 glyphs: each row is 5 bits, MSB = left column. Used where a 3x5 mark dies at 64 px.
FONT_BIG = {
    "?": [0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100],
    "!": [0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100],
    "Z": [0b11111, 0b00010, 0b00100, 0b01000, 0b11111, 0b00000, 0b00000],
}


def iround(v):
    return int(math.floor(v + 0.5))


def hash2(x, y):
    h = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return h ^ (h >> 16)


class Roles:
    """Named colour roles, resolved to a face's dynamic-palette indices."""

    def __init__(self, **kw):
        self.__dict__.update(kw)


class Frame:
    def __init__(self):
        self.pixels = {}

    def px(self, x, y, c):
        if 0 <= x < 64 and 0 <= y < 64:
            self.pixels[y * 64 + x] = c

    def ln(self, x0, y0, x1, y1, c):
        n = max(abs(x1 - x0), abs(y1 - y0))
        for i in range(n + 1):
            self.px(iround(x0 + (x1 - x0) * i / n) if n else x0,
                    iround(y0 + (y1 - y0) * i / n) if n else y0, c)

    def glyph(self, x, y, ch, c):
        rows = FONT[ch]
        for r in range(5):
            for b in range(3):
                if rows[r] & (4 >> b):
                    self.px(x + b, y + r, c)

    def glyph_big(self, x, y, ch, c):
        rows = FONT_BIG[ch]
        for r in range(7):
            for b in range(5):
                if rows[r] & (16 >> b):
                    self.px(x + b, y + r, c)

    def box2(self, x, y, c):
        """A 2x2 block: the smallest thing that still reads as deliberate at 64 px."""
        self.px(x, y, c); self.px(x + 1, y, c)
        self.px(x, y + 1, c); self.px(x + 1, y + 1, c)

    def dot_ring(self, cx, cy, rad, c, step=6, phase=0.0):
        for deg in range(0, 360, step):
            a = (deg + phase) * TAU / 360.0
            self.px(iround(cx + math.cos(a) * rad), iround(cy + math.sin(a) * rad), c)

    def ray(self, cx, cy, a, r0, r1, c):
        for r in range(int(r0), int(r1) + 1):
            self.px(iround(cx + math.cos(a) * r), iround(cy + math.sin(a) * r), c)

    def disk(self, cx, cy, rad, c):
        for dy in range(-rad, rad + 1):
            for dx in range(-rad, rad + 1):
                if dx * dx + dy * dy <= rad * rad + 0.5:
                    self.px(cx + dx, cy + dy, c)

    def spark(self, x, y, c, s=1):
        """A 4-point twinkle: a plus of arm length s, with diagonals when s >= 2."""
        self.px(x, y, c)
        for k in range(1, s + 1):
            self.px(x + k, y, c); self.px(x - k, y, c)
            self.px(x, y + k, c); self.px(x, y - k, c)
        if s >= 2:
            self.px(x + 1, y + 1, c); self.px(x - 1, y - 1, c)
            self.px(x + 1, y - 1, c); self.px(x - 1, y + 1, c)

    def flake(self, x, y, c, r_):
        """A tiny asterisk snowflake."""
        self.px(x, y, c)
        self.px(x + 1, y, c); self.px(x - 1, y, c)
        self.px(x, y + 1, c); self.px(x, y - 1, c)
        if r_:
            self.px(x + 1, y + 1, c); self.px(x - 1, y - 1, c)
            self.px(x + 1, y - 1, c); self.px(x - 1, y + 1, c)

    def rect(self, x0, y0, x1, y1, c):
        self.ln(x0, y0, x1, y0, c); self.ln(x0, y1, x1, y1, c)
        self.ln(x0, y0, x0, y1, c); self.ln(x1, y0, x1, y1, c)

    def halo(self, t, k, c, cx=31.5, cy=31.0, rx=21.0, ry=25.0):
        pulse = math.sin(t * 7.0) * 1.2
        bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
        for i in range(0, 360, 3):
            a = i * TAU / 360.0
            x, y = iround(cx + math.cos(a) * (rx + pulse)), iround(cy + math.sin(a) * (ry + pulse))
            if bayer[(i // 3) & 3][((i // 3) >> 2) & 3] < int(k * 16.0):
                self.px(x, y, c)


def in_disc(x, y):
    """The firmware vignette keeps only d <= 29 fully opaque (see the golem baker's note)."""
    return math.hypot(x - 31.5, y - 31.5) <= 29.0


# ---- the 52 reactions ----------------------------------------------------------------------
# rt = seconds into the reaction, t = absolute time, r = Roles. Props are sky-first.
# Keep every stroke inside d <= 27.5 of (31.5, 31.5) unless it is meant to drift in/out.

def g_smirk(f, rt, t, r):
    # a real glint cluster: big twinkle + a fading pair (the lone 3 px spark read as dust)
    if 0.4 <= rt < 2.4:
        s = 3 if int(rt * 8) % 4 != 3 else 1
        f.spark(52, 22, r.WHITE if s == 3 else r.G0, s)
        f.px(55, 17, r.WHITE); f.px(54, 19, r.G1)


def g_scan(f, rt, t, r):
    # C5: the head top is y7 -- start the bar on the crown, never floating above it
    y = 8 + int(rt / 2.6 * 52)
    for x in range(8, 56):
        f.px(x, y, r.WHITE if x % 2 else r.G0)
        f.px(x, y + 1, r.G0 if x % 2 else r.WHITE)
    for k, c in ((2, r.G2), (3, r.G3), (4, r.G3)):
        for x in range(8, 56, 2):
            f.px(x, y - k, c)


def g_alert(f, rt, t, r):
    f.dot_ring(31.5, 31.5, 26 + (rt / 2.2) * 3, r.G1, step=8)
    if int(rt * 6) % 2 == 0:
        f.glyph(12, 10, "!", r.WHITE)


def g_glitch(f, rt, t, r):
    step = int(rt / 0.15)
    for k in range(4):
        y = 8 + hash2(step, k) % 48
        x0 = 6 + hash2(step, k + 9) % 30
        w = 6 + hash2(step, k + 17) % 20
        off = hash2(step, k + 3) % 7 - 3
        for x in range(x0, x0 + w):
            f.px(x + off, y, r.WHITE if k % 2 else r.G0)


def g_squint(f, rt, t, r):
    # brow-tension ticks beside the eyes; the old side dashes sat in the vignette crop
    k = min(rt / 0.8, 1.0)
    lift = 2 - iround(k * 2)
    for x, s in ((17, -1), (44, 1)):
        f.ln(x, 22 - lift, x + s * 3, 22 - lift, r.G1)
        f.px(x + s * 3, 23 - lift, r.G2)


def g_angry(f, rt, t, r):
    for x0 in (13, 50):
        for p in range(3):
            ph = (rt * 0.9 + p / 3.0) % 1.0
            f.disk(x0, int(20 - ph * 14), 1 + int(ph * 2), r.G2 if ph < 0.6 else r.G3)
    if int(rt * 4) % 2 == 0:
        f.spark(46, 10, r.ACC, 2)
        f.spark(56, 24, r.ACC, 1)


def g_surprised(f, rt, t, r):
    if rt > 0.1:
        ext = min((rt - 0.1) * 16, 4)
        for deg in range(200, 341, 28):
            f.ray(31.5, 31.5, math.radians(deg), 25, 25 + ext, r.WHITE)
    if int(rt * 5) % 2 == 0:
        f.glyph(15, 12, "!", r.G0)      # was (10,12): clipped by the disc
        f.glyph(46, 12, "!", r.G0)      # was (51,12): clipped by the disc


def g_pleased(f, rt, t, r):
    for i, x0 in enumerate((14, 47)):
        for k in range(2):
            ph = (rt / 2.6 + k * 0.5 + i * 0.25) % 1.0
            s = 2 if ph < 0.35 else (1 if ph < 0.7 else 0)
            f.spark(x0 + (1 if ph > 0.5 else 0), int(50 - ph * 34),
                    r.WHITE if int(rt * 6 + k) % 2 == 0 else r.G1, s)


def g_laugh(f, rt, t, r):
    bob = iround(math.sin(t * 14) * 1.5)
    for side in (-1, 1):
        base = 58 if side > 0 else 5
        for k, dy in enumerate((-6, 0, 6)):
            f.ln(base + side * bob, 31 + dy, base - side * 3 + side * bob, 31 + dy,
                 r.G0 if k == 1 else r.G1)


def g_wink(f, rt, t, r):
    if 0.2 <= rt < 1.5:
        s = 2 if math.sin((rt - 0.2) / 1.3 * math.pi) > 0.6 else 1
        f.spark(50, 18, r.WHITE if s == 2 else r.G0, s)   # was (52,12): clipped
        f.px(54, 22, r.G1); f.px(55, 23, r.G0)


def g_sleepy(f, rt, t, r):
    # one big Z rising + two smaller ones trailing, all inside the disc
    ph = (rt / 3.4 * 2) % 1.0
    f.glyph_big(44 + int(ph * 2), int(24 - ph * 14), "Z", r.G1)
    for k in range(2):
        ph2 = (rt / 3.4 * 2 + (k + 1) / 3.0) % 1.0
        if ph2 < 0.85:
            f.glyph(48 + k * 2, int(22 - ph2 * 10), "Z", r.G2 if k == 0 else r.G1)
    f.px(17, 15, r.COOL); f.px(16, 16, r.COOL); f.px(15, 15, r.COOL)


def g_confused(f, rt, t, r):
    # a 5x7 bobbing "?" (the 3x5 one dissolved into specks) + two orbit rings
    f.glyph_big(45, 10 + iround(math.sin(t * 5) * 1.5), "?", r.WHITE)
    for k in range(8):
        a = k * TAU / 8 + rt * 5
        f.px(iround(15 + math.cos(a) * 2.5), iround(15 + math.sin(a) * 2.5), r.G1)
        f.px(iround(15 + math.cos(a) * 3.5), iround(15 + math.sin(a) * 3.5), r.G2 if k % 2 else r.G1)


def g_evil(f, rt, t, r):
    if rt > 0.5:
        f.dot_ring(31.5, 31.5, 27.0, r.ACC, step=12, phase=t * 100)
        f.dot_ring(31.5, 31.5, 28.5, r.G2, step=12, phase=90 - t * 60)
    if rt > 0.8 and int(rt * 2) % 2 == 0:
        for x, y in ((6, 16), (57, 16), (6, 46), (57, 46)):
            f.ln(x, y, x + 2, y - 2, r.ACC)


def g_hacked(f, rt, t, r):
    for ci, x in enumerate((8, 15, 48, 55)):
        for k in range(4):
            y = (int(rt * 42 + ci * 13 - k * 3)) % 56 + 4
            f.px(x, y, r.WHITE if k == 0 else r.G0)
            f.px(x + 1, y, r.G1)
    if int(rt * 8) % 6 == 0:
        y = 7 + (int(rt * 19) % 52)
        f.ln(4, y, 59, y, r.WHITE)


def g_powerup(f, rt, t, r):
    for k in range(3):
        rr = (rt * 14 + k * 9) % 26 + 3
        f.dot_ring(31.5, 31.5, rr, r.G0 if rr < 16 else r.G2, step=6)
    for x in (8, 55):
        for k in range(3):
            y = 60 - int((rt * 30 + k * 11) % 40)
            f.px(x, y, r.G0)
            f.px(x, y + 1, r.G2)


def g_bored(f, rt, t, r):
    # a thought bubble whose dots land one by one (the loose dots read as stray sparkles)
    f.rect(43, 11, 51, 17, r.WHITE)
    f.px(44, 18, r.WHITE); f.px(45, 19, r.WHITE)
    for k in range(3):
        if rt > 0.4 + k * 0.5:
            f.box2(45 + k * 2, 13, r.WHITE)


def g_disgust(f, rt, t, r):
    # two 2 px stink squiggles rising in front of the face (they were 1 px strokes)
    for k, x0 in enumerate((22, 41)):
        for y in range(20, 7, -1):
            x = x0 + iround(math.sin((y - rt * 18 + k * 5) * 0.9) * 1.6)
            f.px(x, y, r.G1 if (y + k) % 2 == 0 else r.G2)
            f.px(x + 1, y, r.G2 if (y + k) % 2 == 0 else r.G1)


def g_target(f, rt, t, r):
    k = min(rt / 1.2, 1.0)
    f.dot_ring(31.5, 31.5, 27 - k * 9, r.G1, step=7)
    if rt > 1.2:
        c = r.WHITE if int(rt * 8) % 2 == 0 else r.G0
        for deg in (0, 90, 180, 270):
            f.ray(31.5, 31.5, math.radians(deg), 22, 27, c)


def g_nervous(f, rt, t, r):
    # blue 2x2 sweat drops falling down the temples (yellow ticks read as glitch)
    for x0, ph in ((14, 0.0), (49, 0.5)):
        y = int(12 + ((rt / 2.6 + ph) % 1.0) * 14)
        f.box2(x0, y, r.COOL)
    jog = 1 if int(rt * 10) % 2 == 0 else -1
    for k in range(3):
        f.ln(16 + jog, 26 + k * 4, 19 + jog, 26 + k * 4, r.G1)


def g_proud(f, rt, t, r):
    # a chest-high glint "medal": a growing star with a short ribbon flick (EMBED: on ink only)
    k = min(rt / 2.8, 1.0)
    sx = 44 + iround(k * 3)
    f.spark(sx, 17, r.WHITE, 2)
    f.ln(sx - 3, 20, sx - 6, 23, r.G1)
    f.px(sx - 4, 24, r.G1)
    if 0.5 <= rt < 1.6:
        f.spark(15, 14, r.WHITE, 1)


def g_excited(f, rt, t, r):
    for k in range(8):
        a = k * TAU / 8 + t * 2.2
        x, y = iround(31.5 + math.cos(a) * 26), iround(31.5 + math.sin(a) * 26)
        f.spark(x, y, r.WHITE if (k + int(rt * 4)) % 2 == 0 else r.G0,
                1 if int(rt * 6 + k) % 3 else 0)


def g_confetti(f, rt, t, r):
    # 2x2 bits (1 px bits read as dust) plus two short held streamers
    cols = (r.G0, r.WHITE, r.RB0, r.RB3, r.RB4)   # C5 body is all warm: carry the party in pop colours
    for k in range(26):
        x = 6 + hash2(k, 3) % 52
        ph = (rt / 3.4 + (hash2(k, 5) % 100) / 100.0) % 1.0
        y = int(2 + ph * 58)
        f.box2(x, y, cols[hash2(k, 7) % 5])
    for k in range(4):
        ph = (rt / 3.4 * 1.5 + k * 0.25) % 1.0
        if ph < 0.6:
            x = 15 + (k % 2) * 28
            y = int(10 + ph * 30)
            c = (r.RB1, r.RB2, r.RB0, r.RB3)[k]
            f.px(x, y, c); f.px(x, y + 1, c); f.px(x + 1, y, c)


def g_sunny(f, rt, t, r):
    # the sun moved in from the corner (its outer rays were cropped and it landed on
    # the stone's helmet as bare glints); (17,16) keeps every ray inside the disc
    sx, sy = 17, 16
    for i in range(12):
        a = i * TAU / 12.0 + t * 1.35
        ln = 5 if i % 2 else 6
        for rr in range(3, ln + 1):
            f.px(iround(sx + math.cos(a) * rr), iround(sy + math.sin(a) * rr * 0.92),
                 r.WHITE if rr >= ln else (r.G0 if rr > 3 else r.G1))
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            d2 = dx * dx + dy * dy
            if d2 <= 5.5:
                f.px(sx + dx, sy + dy, r.WHITE if d2 < 2.0 else r.G0)


def g_rain(f, rt, t, r):
    for k in range(14):
        x = 5 + hash2(k, 1) % 54
        ph = (t * 1.8 + (hash2(k, 2) % 100) / 100.0) % 1.0
        y = int(ph * 60)
        f.ln(x, y, x - 2, y - 4, r.COOL)
        f.px(x, y, r.WHITE)


def g_storm(f, rt, t, r):
    for x in range(20, 47, 3):
        f.px(x, 11, r.G2); f.px(x + 1, 11, r.G2)
    if (rt % 1.1) < 0.22:
        pts = [(36, 12), (31, 18), (35, 18), (30, 25), (34, 25), (29, 32)]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            # a G2 shadow line under the white: a bare white bolt vanished on the
            # C5 cream plate (white-on-cream); the shadow carries it on pale ground
            f.ln(x0 + 1, y0 + 1, x1 + 1, y1 + 1, r.G2)
            f.ln(x0, y0, x1, y1, r.WHITE)
    for k in range(12):
        x = 5 + hash2(k, 1) % 54
        ph = (t * 2.6 + (hash2(k, 2) % 100) / 100.0) % 1.0
        y = int(ph * 60)
        f.ln(x, y, x - 1, y - 3, r.COOL)


def g_cold(f, rt, t, r):
    # full asterisk flakes orbiting inside the disc, plus a collective shiver
    shiv = 1 if int(rt * 2.2) % 2 == 0 else -1
    for k in range(5):
        a = k * TAU / 5 + rt * 0.8
        x = iround(31.5 + math.cos(a) * 25.0) + shiv
        y = iround(31.5 + math.sin(a) * 25.0)
        f.flake(x, y, r.WHITE if k % 2 == 0 else r.COOL, 1 if k % 2 == 0 else 0)
        if k % 2 == 0:
            f.flake(x + shiv * 2, y + 3, r.COOL, 0)


def g_rainbow(f, rt, t, r):
    arcs = (r.RB0, r.RB1, r.RB2, r.RB3, r.RB4)
    for i, c in enumerate(arcs):
        if rt > 0.2 + i * 0.25:
            rad = 25 - i * 1.2
            for deg in range(200, 341, 4):
                f.px(iround(31.5 + math.cos(math.radians(deg)) * rad),
                     iround(31.5 + math.sin(math.radians(deg)) * rad), c)


def g_searching(f, rt, t, r):
    # a real magnifier: dense lens ring + 2 px handle + glass glint (was a 20-dot ring)
    cx = 42 + iround(math.cos(t * 3) * 4)
    cy = 15 + iround(math.sin(t * 3) * 3)
    f.dot_ring(cx, cy, 4, r.G0, step=15)
    f.ln(cx + 4, cy + 4, cx + 8, cy + 8, r.G1)
    f.ln(cx + 5, cy + 4, cx + 8, cy + 7, r.G1)
    f.px(cx - 1, cy - 1, r.WHITE); f.px(cx - 2, cy - 1, r.WHITE); f.px(cx - 1, cy - 2, r.WHITE)


def g_browsing(f, rt, t, r):
    # a window with a title bar, scrolling text lines and a cursor (moved in from the corner)
    if rt > 0.2:
        f.rect(14, 40, 30, 54, r.G1)
        f.ln(14, 43, 30, 43, r.G2)
        for i in range(3):
            f.ln(16, 46 + i * 2, 16 + (int(rt * 9 + i * 5) % 12), 46 + i * 2, r.G2 if i else r.G0)
        cx = 17 + iround((rt * 9) % 11)
        f.px(cx, 53, r.WHITE)


def g_reading(f, rt, t, r):
    # an actual book: two pages, a spine, a turning page (was floating text lines, cropped)
    f.rect(34, 42, 48, 54, r.G1)
    f.ln(41, 42, 41, 54, r.G0)
    for i in range(2):
        f.ln(36, 45 + i * 3, 39, 45 + i * 3, r.G2)
        f.ln(43, 45 + i * 3, 46, 45 + i * 3, r.G2)
    flip = int((rt * 4) % 3)
    f.ln(41 + flip, 44, 41 + flip, 52, r.WHITE)


def g_writing(f, rt, t, r):
    # a page with a line growing stroke by stroke and a visible 2x2 pen nib
    f.rect(16, 44, 30, 56, r.G1)
    n = int(min(rt / 2.8, 1.0) * 14)
    for i in range(n):
        f.px(18 + (i % 7) * 2, 49 + (i // 7) * 4, r.G0)
    px, py = 18 + (min(n, 13) % 7) * 2, 48 + (min(n, 13) // 7) * 4
    f.box2(px, py, r.WHITE)


def g_checking(f, rt, t, r):
    # clipboard + a 2 px tick inside the disc (was half-cropped at the top-right corner)
    f.rect(38, 12, 50, 24, r.G1)
    if rt > 0.4:
        f.ln(41, 18, 44, 22, r.WHITE); f.ln(41, 19, 44, 22, r.WHITE)
    if rt > 0.9:
        f.ln(44, 22, 49, 15, r.WHITE); f.ln(45, 22, 49, 16, r.WHITE)


def g_calculating(f, rt, t, r):
    # floating glyphs pulled in to x=13/46 (the x=4 column was 100% cropped)
    items = ("+", "=", "1", "0", "+", "1")
    for k, ch in enumerate(items):
        ph = (rt / 3.0 + k / 6.0) % 1.0
        x = 13 if k % 2 == 0 else 46
        y = int(48 - ph * 34)
        if ph < 0.8:
            f.glyph(x, y, ch, r.G1 if k % 3 else r.G0)


def g_calendar(f, rt, t, r):
    f.rect(40, 32, 52, 46, r.G1)
    f.ln(40, 35, 52, 35, r.G0)
    day = int((rt * 4) % 6)
    f.box2(42 + (day % 3) * 3, 38 + (day // 3) * 3, r.WHITE)


def g_monitoring(f, rt, t, r):
    # a radar scope fully inside the disc: ring + a range arc + a bright sweep
    f.dot_ring(17, 17, 5, r.G1, step=12)
    f.dot_ring(17, 17, 3, r.G2, step=24)
    a = t * 2.4
    for rr in range(1, 6):
        f.px(iround(17 + math.cos(a) * rr), iround(17 + math.sin(a) * rr), r.G0)
    if int(rt * 3) % 2 == 0:
        f.px(iround(17 + math.cos(a + 0.9) * 3), iround(17 + math.sin(a + 0.9) * 3), r.WHITE)


def g_generating(f, rt, t, r):
    # a canvas on an easel with paint dabs (differentiates it from invoking's wand)
    cols = (r.G0, r.G1, r.G2, r.G3)
    f.rect(30, 10, 42, 24, r.G1)
    f.ln(30, 24, 27, 28, r.G1); f.ln(31, 24, 28, 28, r.G2)
    f.ln(42, 24, 45, 28, r.G1); f.ln(41, 24, 44, 28, r.G2)
    for k in range(int(rt * 6) + 1):
        x = 32 + hash2(k, 21) % 8
        y = 13 + hash2(k, 22) % 9
        f.box2(x, y, cols[k % 4])
    if int(rt * 4) % 2 == 0:
        f.px(43, 14, r.WHITE)


def g_running(f, rt, t, r):
    # a terminal with 2 px text lines and a typing cursor (the old frame sat in the crop)
    f.rect(34, 40, 48, 54, r.G1)
    f.ln(34, 43, 48, 43, r.G2)
    for k in range(3):
        if rt > 0.4 + k * 0.6:
            xe = 36 + int((rt * 8) % 10)
            f.ln(36, 46 + k * 3, xe, 46 + k * 3, r.G0)
            f.ln(36, 47 + k * 3, xe, 47 + k * 3, r.G0)
    if int(rt * 4) % 2 == 0:
        f.box2(46, 51, r.WHITE)


def g_invoking(f, rt, t, r):
    # the wand moved inside the disc (only its tip used to survive) and thickened
    f.ln(18, 54, 27, 45, r.G1); f.ln(19, 54, 28, 45, r.G2)
    for k in range(3):
        f.px(30 + k * 3, 42 - k * 3, r.G0)
    f.spark(29, 43, r.WHITE if int(rt * 6) % 2 == 0 else r.G0, 2)


def g_delegating(f, rt, t, r):
    # three packets dispatched outward from the chest, with short trails
    for k in range(3):
        ph = (rt / 2.8 + k / 3.0) % 1.0
        if ph > 0.85:
            continue
        a = k * TAU / 3 + 1.2
        x = iround(31.5 + math.cos(a) * (6 + ph * 18))
        y = iround(31.5 + math.sin(a) * (6 + ph * 16))
        f.box2(x, y, r.WHITE)
        for j in (1, 2, 3):
            f.px(iround(31.5 + math.cos(a) * (6 + ph * 18 - j * 3)),
                 iround(31.5 + math.sin(a) * (6 + ph * 16 - j * 3)),
                 r.G0 if j == 1 else (r.G1 if j == 2 else r.G2))
        if ph > 0.7:
            f.spark(x, y, r.WHITE, 1)


def g_remembering(f, rt, t, r):
    # a proper bulb: glass ring, base, filament, rays (was a 3 px puff)
    on = rt > 0.8 or int(rt * 9) % 2 == 0
    cx, cy = 46, 14
    f.dot_ring(cx, cy, 4, r.G1 if on else r.G2, step=15)
    f.ln(cx - 1, cy + 5, cx + 1, cy + 5, r.G2)
    f.px(cx, cy + 6, r.G2)
    if on:
        f.spark(cx, cy, r.WHITE, 1)
        for deg in (210, 270, 330):
            f.ray(cx, cy, math.radians(deg), 5, 6, r.WHITE)


def g_shrug(f, rt, t, r):
    # raised "palms" at the shoulders inside the disc (the old strokes were fully cropped)
    lift = iround(math.sin(min(rt / 0.5, 1.0) * math.pi / 2) * 3)
    for side in (-1, 1):
        x0 = 31 + side * 17
        f.ln(x0, 40 - lift, x0 - side * 3, 43 - lift, r.G1)
        f.ln(x0, 41 - lift, x0 - side * 3, 44 - lift, r.G1)
    if rt > 0.5:
        f.glyph(13, 30, "?", r.G0)


def g_embarrassed(f, rt, t, r):
    # a fluster swirl fully inside the disc (was 80% cropped) + one sweat drop
    for k in range(10):
        a = k * 0.6 + rt * 4
        rr = 1 + k * 0.22
        f.px(iround(16 + math.cos(a) * rr), iround(15 + math.sin(a) * rr), r.WHITE if k % 3 else r.G0)
    f.box2(46, 13, r.COOL)


def g_scared(f, rt, t, r):
    # trembling jitter strokes at the temples (were cropped) + blue drops
    jit = 1 if int(rt * 12) % 2 == 0 else -1
    for side in (-1, 1):
        for k in range(3):
            x = 31 + side * 20
            f.ln(x + jit, 14 + k * 8, x + side * 2 + jit, 16 + k * 8, r.WHITE)
    f.box2(50, 18, r.COOL)
    f.px(49, 16, r.COOL)


def g_cool(f, rt, t, r):
    x0 = 10 + iround(min(rt / 2.8, 1.0) * 36)
    for i in range(10):
        f.px(x0 + i, 10 + i // 2, r.WHITE)
        f.px(x0 + i, 11 + i // 2, r.WHITE if i < 5 else r.G1)
    if int(rt * 5) % 3 == 0:
        f.spark(54, 18, r.WHITE, 1)
        f.spark(8, 34, r.G0, 1)


def g_mind_blown(f, rt, t, r):
    rad = min(rt * 14, 28)
    for k in range(8):
        a = k * TAU / 8
        f.ray(31.5, 31.5, a, rad - 3, rad, r.WHITE if k % 2 == 0 else r.G0)
    if rt < 0.8:
        f.dot_ring(31.5, 31.5, 24 + rt * 8, r.G1, step=9)


def g_crying_laughing(f, rt, t, r):
    # 2 px tears (1 px flecks read as motion noise) + the rocking jitter
    ph = (rt * 1.2) % 1.0
    for x in (12, 49):
        y = 26 + int(ph * 14)
        f.px(x, y, r.COOL); f.px(x, y + 1, r.COOL); f.px(x + 1, y, r.COOL)
    jit = 1 if int(rt * 8) % 2 == 0 else -1
    for k in range(3):
        f.ln(15 + jit, 32 + k * 3, 18 + jit, 32 + k * 3, r.G1)


def g_sad(f, rt, t, r):
    # the cloud pulled in from the corner so it is not cropped
    f.disk(20, 13, 3, r.COOL); f.disk(24, 13, 3, r.COOL); f.disk(28, 13, 2, r.COOL)
    f.ln(17, 15, 29, 15, r.COOL)
    for k in range(3):
        ph = (rt * 1.1 + k / 3.0) % 1.0
        y = int(16 + ph * 10)
        f.px(21 + k * 3, y, r.COOL); f.px(21 + k * 3, y + 1, r.COOL)


def g_nod(f, rt, t, r):
    # chevrons pulled in from the cropped corners and thickened
    bob = iround(math.sin(rt * TAU / 0.6) * 2)
    for x in (15, 47):
        f.ln(x, 12 + bob, x + 3, 16 + bob, r.WHITE)
        f.ln(x + 3, 16 + bob, x + 6, 12 + bob, r.WHITE)
        f.ln(x, 13 + bob, x + 1, 17 + bob, r.G1)


def g_cloudy(f, rt, t, r):
    # white clouds (olive puffs vanished against the body) drifting through the top band
    for base in (0, 32):
        cx = (base + int(rt * 6)) % 64 - 8
        body = r.G0      # G0 is near-white on both faces (WHITE renders as the stone's dark ink)
        f.disk(cx + 4, 10, 3, body); f.disk(cx + 8, 9, 3, body); f.disk(cx + 12, 10, 2, body)
        f.ln(cx + 2, 12, cx + 13, 12, r.G2)


def g_windy(f, rt, t, r):
    # 2 px tapered streaks (1 px dashes read as scratches) + a tumbling leaf
    for k in range(3):
        y = 11 + k * 7
        x = (int(rt * 22) + k * 11) % 56 + 6
        f.ln(x, y, x + 6, y, r.G0)
        f.ln(x + 1, y + 1, x + 7, y + 1, r.G1)
    ph = (rt / 3.0) % 1.0
    lx = iround(14 + ph * 32)
    f.px(lx, 8, r.G0); f.px(lx + 1, 8, r.G0); f.px(lx + 2, 8, r.WHITE); f.px(lx + 1, 9, r.G1)


def g_hot(f, rt, t, r):
    # heat squiggles pulled in from the corners and doubled to 2 px
    for k, x0 in enumerate((15, 21, 43, 49)):
        for y in range(22, 9, -1):
            x = x0 + iround(math.sin((y - rt * 14 + k * 3) * 0.8) * 1.5)
            f.px(x, y, r.WHITE if y % 3 == 0 else r.G1)
            f.px(x + 1, y, r.G2)


def g_snow(f, rt, t, r):
    for k in range(16):
        x = 5 + hash2(k, 4) % 54 + iround(math.sin(t * 2 + k))
        ph = (rt / 3.4 + (hash2(k, 6) % 100) / 100.0) % 1.0
        y = int(ph * 62)
        f.flake(x, y, r.WHITE, 1 if k % 3 == 0 else 0)


# name -> generator, in the relay's MOODS order
MOODS = [
    ("smirk", g_smirk), ("scan", g_scan), ("alert", g_alert), ("glitch", g_glitch),
    ("squint", g_squint), ("angry", g_angry), ("surprised", g_surprised), ("pleased", g_pleased),
    ("laugh", g_laugh), ("wink", g_wink), ("sleepy", g_sleepy), ("confused", g_confused),
    ("evil", g_evil), ("hacked", g_hacked), ("powerup", g_powerup), ("bored", g_bored),
    ("disgust", g_disgust), ("target", g_target), ("nervous", g_nervous), ("proud", g_proud),
    ("excited", g_excited), ("confetti", g_confetti), ("sunny", g_sunny), ("rain", g_rain),
    ("storm", g_storm), ("cold", g_cold), ("rainbow", g_rainbow),
    ("searching", g_searching), ("browsing", g_browsing), ("reading", g_reading),
    ("writing", g_writing), ("checking", g_checking), ("calculating", g_calculating),
    ("calendar", g_calendar), ("monitoring", g_monitoring), ("generating", g_generating),
    ("running", g_running), ("invoking", g_invoking), ("delegating", g_delegating),
    ("remembering", g_remembering),
    ("shrug", g_shrug), ("embarrassed", g_embarrassed), ("scared", g_scared), ("cool", g_cool),
    ("mind_blown", g_mind_blown), ("crying_laughing", g_crying_laughing), ("sad", g_sad),
    ("nod", g_nod),
    ("cloudy", g_cloudy), ("windy", g_windy), ("hot", g_hot), ("snow", g_snow),
]

# body-anchored props: written ON the character, so keep only pixels on sprite ink
EMBED = ("glitch", "proud", "cool")

FACES = {
    # sprite header, the face's dynamic palette, and the C side's name of each role
    "golem": {"sprite": "sprite_golem.h", "roles": lambda n: Roles(
        G0=n, G1=n + 1, G2=n + 2, G3=n + 3, ACC=n + 4, WHITE=n + 5,
        RB0=n + 6, RB1=n + 7, RB2=n + 8, RB3=n + 9, RB4=n + 10, RB5=n + 11, RB6=n + 12, COOL=n + 13)},
    # the stone is pale grey, so a WHITE highlight vanishes on it: WHITE is the dark outline ink
    # (sprite index 1, IDX_DARK) and the glow ramp carries the colour
    "stone": {"sprite": "sprite_stone.h", "roles": lambda n: Roles(
        G0=n, G1=n + 1, G2=n + 2, G3=n + 3, ACC=n + 4, WHITE=1,
        RB0=n, RB1=n + 1, RB2=n + 2, RB3=n + 3, RB4=n + 4, RB5=1, RB6=n, COOL=n + 1)},
}


def load_sprite(path):
    text = open(path).read()
    n = int(re.search(r"#define SPR_N (\d+)", text).group(1))
    vals = re.search(r"SPR\[64 \* 64\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    spr = np.array([int(v) for v in vals.replace("\n", " ").split(",") if v.strip()]).reshape(64, 64)
    return n, spr != 0


def bake(face, out_path):
    spec = FACES[face]
    spr_n, body = load_sprite(os.path.join(AVATAR, spec["sprite"]))
    roles = spec["roles"](spr_n)
    table, counts, px_flat, col_flat = [], [], [], []
    frame_off = 0
    for name, fn in MOODS:
        dur = DURS[name]
        n_frames = int(round(dur * 1000 / FRAME_MS))
        table.append((name, n_frames, frame_off))
        frame_off += n_frames
        for fi in range(n_frames):
            rt = fi * FRAME_MS / 1000.0
            f = Frame()
            fn(f, rt, 100.0 + rt, roles)
            pix = {p: c for p, c in f.pixels.items() if in_disc(p % 64, p // 64)}
            if name in EMBED:
                pix = {p: c for p, c in pix.items() if body[p // 64, p % 64]}
            counts.append(len(pix))
            for idx, c in sorted(pix.items()):
                px_flat.append(idx)
                col_flat.append(c)
    with open(out_path, "w") as fo:
        fo.write("// Generated by bake_moods.py (%s): all %d relay reactions as baked sparse diffs.\n" % (face, len(MOODS)))
        fo.write("// Do not edit by hand. Frame colours are ABSOLUTE indices in the renderer's dynamic palette.\n")
        fo.write("#pragma once\n#include <stdint.h>\n")
        fo.write("#define SPR_N %d\n" % spr_n)
        fo.write("#define MOOD_FRAME_DT_MS %d\n#define MOOD_FRAME_COUNT %d\n" % (FRAME_MS, len(counts)))
        fo.write("static const struct { char name[16]; uint8_t n; uint16_t off; } MOODS[] = {\n")
        for name, n_frames, off in table:
            fo.write('    { "%s", %d, %d },\n' % (name, n_frames, off))
        fo.write("};\n")
        fo.write("static const uint16_t MOOD_FRAME_COUNTS[] = {\n")
        fo.write(",\n".join("    %d" % c for c in counts) + ",\n};\n")
        fo.write("static const uint16_t MOOD_FRAME_PX[] = {\n")
        for idx, c in zip(px_flat, col_flat):
            fo.write("    %d, %d,\n" % (idx, c))
        fo.write("};\n")
        fo.write("static const uint8_t MOOD_FRAME_C[] = {\n")
        fo.write(",\n".join("    %d" % c for c in col_flat) + ",\n};\n")
    print("%s: %d moods, %d frames, %d diffs -> %s" % (face, len(table), len(counts), len(px_flat), out_path))


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in FACES:
        sys.exit(__doc__)
    bake(sys.argv[1], sys.argv[2])
