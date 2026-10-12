"""golem_char.py - the C5 terracotta automaton composed for the golem renderer.

Loads the approved C5 64x64 snap and produces the FINAL baked sprite:

  - quantised to a curated 10-colour palette (the machinery predicates are
    checked in report(): goldish/darkish/neutral/luminance-rank);
  - both eyes REDRAWN for the machinery: dark ring (socket), amber iris annulus,
    dark pupil, light glint; the lens pixels (kind 2) are the whole inner disc
    (pupil+iris+glint) so gaze/blink/half/wide move it as one unit;
  - a chest core seal added at (31.5, 51.5) (the renderer's voice organ breathes
    and thinks there);
  - the carved smile (x28..35, y37..38) kept as-is - it is the closed mouth now;
  - kinds: 2 = lens, 1 = clay glints (a dozen rust speckles on the shell),
    3 = clay shimmer (dark shell mottle).

SPR/PAL/KIND are module-level; the header writer and the mood baker read them.
"""
import math
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "golem_c5_64.png")

# ---- the curated palette ------------------------------------------------------
# name, rgb, why the value is what it is
PAL_SPEC = [
    ("BLACK", (0, 0, 0)),          # bg / glitch fills
    ("DEEP", (12, 11, 30)),       # ring, socket, pupil (nudged 14,13,33: max ch 30 < 35.7 -> darkish)
    ("PLATE", (245, 233, 218)),   # cream face plate (nudged 237,226,193: s=0.11 -> pinned near-white role)
    ("TAN", (209, 173, 131)),     # cheeks / lit edges
    ("RUST", (104, 50, 42)),      # dark rust: weathered lines, smile
    ("TERRA_D", (163, 80, 54)),   # shell dark
    ("TERRA_M", (194, 101, 63)),  # shell mid
    ("TERRA_L", (216, 120, 83)),  # shell light
    ("IRIS", (232, 142, 70)),     # the amber lens
    ("GLINT", (255, 246, 230)),   # the lens highlight
]
NAMES = [n for n, _ in PAL_SPEC]
PAL = np.array([c for _, c in PAL_SPEC], np.uint8)
IDX = {n: i for i, n in enumerate(NAMES)}
SPR_N = len(PAL_SPEC)

EYE_CX = (21.5, 41.5)   # symmetric about x=31.5, same as the approved C5 art
EYE_CY = 29.0
EYE_RX, EYE_RY = 6.3, 6.5
CORE_CX, CORE_CY = 31.5, 51.5
CORE_RX, CORE_RY = 4.0, 3.6
EYE_BOXES = [(int(cx - EYE_RX) - 1, int(cx + EYE_RX) + 1, int(EYE_CY - EYE_RY) - 1, int(EYE_CY + EYE_RY) + 2) for cx in EYE_CX]  # x0, x1, y0, y1


def _quantise(a):
    """Nearest-palette map (lossless except the two intended nudges)."""
    d = ((a[:, :, None, :].astype(np.int32) - PAL[None, None, :, :].astype(np.int32)) ** 2).sum(-1)
    return d.argmin(-1).astype(np.uint8)


def _draw_eye(idx, kind, cx):
    for y in range(0, 64):
        for x in range(0, 64):
            dx, dy = x - cx, y - EYE_CY
            r = math.hypot(dx / EYE_RX, dy / EYE_RY)
            if r > 1.0:
                continue
            if r >= 0.78:
                idx[y, x] = IDX["DEEP"] if abs(dx / EYE_RX) > 0.72 else IDX["RUST"]
            elif r >= 0.52:
                idx[y, x] = IDX["DEEP"]
            else:
                idx[y, x] = IDX["DEEP"] if r < 0.24 else IDX["IRIS"]
                kind[y, x] = 2
    # the glint: two px at the pupil's top-left (wanders with the lens)
    for gx in (int(cx) - 1, int(cx)):
        idx[27, gx] = IDX["GLINT"]
        kind[27, gx] = 2
    # the under-eye tan arc (C5's cheek highlight)
    for x in range(int(cx) - 3, int(cx) + 4):
        idx[36, x] = IDX["TAN"]


def _draw_core(idx, kind):
    for y in range(0, 64):
        for x in range(0, 64):
            dx, dy = x - CORE_CX, y - CORE_CY
            r = math.hypot(dx / CORE_RX, dy / CORE_RY)
            if r > 1.04:
                continue
            if r >= 0.62:
                idx[y, x] = IDX["DEEP"]
            else:
                idx[y, x] = IDX["RUST"]


def build():
    a = np.array(Image.open(SRC).convert("RGB"))
    assert a.shape[:2] == (64, 64), a.shape
    idx = _quantise(a).astype(np.uint8)
    kind = np.zeros((64, 64), np.uint8)

    for cx in EYE_CX:
        _draw_eye(idx, kind, cx)
    _draw_core(idx, kind)

    # ---- clay glints (kind 1) and clay shimmer (kind 3) on the shell ----------
    def shell_excluded(x, y):
        if y >= 45:                                    # torso: its own thing
            return False
        for x0, x1, y0, y1 in EYE_BOX_GUARD:
            if x0 <= x <= x1 and y0 <= y <= y1:
                return True
        return False

    EYE_BOX_GUARD = []
    for cx in EYE_CX:
        EYE_BOX_GUARD.append((int(cx - EYE_RX) - 1, int(cx + EYE_RX) + 1, int(EYE_CY - EYE_RY) - 1, int(EYE_CY + EYE_RY) + 3))
    n1 = 0
    for y in range(4, 45):
        for x in range(4, 60):
            if kind[y, x] or shell_excluded(x, y):
                continue
            # the C5 art has no rust speckle field: the clay's dark mottle is TERRA_D.
            # glints ride the darkest specks: every ~16th of the shimmer set, thinned.
            if idx[y, x] == IDX["TERRA_D"] and (x * 5 + y * 3) % 16 == 2 and n1 < 9:
                kind[y, x] = 1
                n1 += 1
    n3 = 0
    for y in range(4, 45):
        for x in range(4, 60):
            if kind[y, x] or shell_excluded(x, y):
                continue
            if idx[y, x] == IDX["TERRA_D"] and (x * 3 + y * 5) % 4 == 0 and n3 < 160:
                kind[y, x] = 3
                n3 += 1
    return idx, kind, n1, n3


SPR, KIND, N_K1, N_K3 = build()


def report():
    """Machinery predicates over the final palette."""
    print("SPR_N=%d  k1(glints)=%d k2(lens)=%d k3(shimmer)=%d body=%d" %
          (SPR_N, N_K1, int((KIND == 2).sum()), N_K3, int((SPR != 0).sum())))
    lum = 0.30 * PAL[:, 0] + 0.70 * PAL[:, 1]
    order = np.argsort(-lum)
    rank = {int(i): r for r, i in enumerate(order)}
    for i, n in enumerate(NAMES):
        r, g, b = PAL[i]
        mx, mn = max(r, g, b), min(r, g, b)
        v = mx / 255.0
        s = (mx - mn) / mx if mx else 0.0
        goldish = r > 60 and r >= b
        darkish = mx < int(0.14 * 255)
        neutral = s <= 0.12
        warm = v > 0.85 and s < 0.30
        print("%-8s idx=%2d rgb=(%3d,%3d,%3d) lum=%6.1f rank=%2d  goldish=%-5s darkish=%-5s neutral=%-5s is_warm=%-5s" %
              (n, i, r, g, b, lum[i], rank[i], goldish, darkish, neutral, warm))
    eye_l = [(x, y) for y in range(64) for x in range(64) if KIND[y, x] == 2 and x < 32]
    eye_r = [(x, y) for y in range(64) for x in range(64) if KIND[y, x] == 2 and x >= 32]
    for tag, pts in (("left", eye_l), ("right", eye_r)):
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        print("lens %s n=%d x%d-%d y%d-%d" % (tag, len(pts), min(xs), max(xs), min(ys), max(ys)))


def save_png(path):
    Image.fromarray(PAL[SPR]).save(path)


if __name__ == "__main__":
    report()
    out = os.path.join(HERE, "golem_c5_final_64.png")
    save_png(out)
    print("wrote", out)
