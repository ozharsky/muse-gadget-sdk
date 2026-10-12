#!/usr/bin/env python3
"""Prop audit: simulate the C paint rule on every baked mood, per face.

For each mood generator in bake_moods.py, this counts how many of its proposed
prop pixels actually SURVIVE the firmware's paint rule (goldish or darkish
under-pixel, inside the disc, body-only for EMBED moods) on a given face sprite.
That is the ground truth of what can appear on the glass — everything else the
player silently drops.

    py -3 prop-lab/prop_audit.py [mood ...]
"""
import importlib.util
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# workspace layout: prop-lab/{art-pipeline/moods_v2/bake_moods.py, muse-gadget-sdk/...}
# Mac layout: ~/muse/art-pipeline/moods_v2/ (bake next to this file, SDK at ~/muse/muse-gadget-sdk)
BAKE = os.path.join(HERE, "art-pipeline", "moods_v2", "bake_moods.py")
AVATAR = os.path.join(HERE, "muse-gadget-sdk", "esp32", "components", "muse", "avatar")
if not os.path.exists(BAKE):
    BAKE = os.path.join(HERE, "bake_moods.py")
if not os.path.isdir(AVATAR):
    AVATAR = os.path.normpath(os.path.join(HERE, "..", "..", "muse-gadget-sdk", "esp32", "components", "muse", "avatar"))

spec = importlib.util.spec_from_file_location("bake_moods", BAKE)
B = importlib.util.module_from_spec(spec)
spec.loader.exec_module(B)


def load_sprite(path):
    text = open(path).read()
    n = int(text.split("#define SPR_N ")[1].split("\n")[0])
    pal = []
    blob = text.split("SPR_PAL[SPR_N][3] = {")[1].split("};")[0]
    for row in blob.strip().split("\n"):
        row = row.strip().strip(",")
        if row.startswith("{"):
            pal.append([int(v) for v in row.strip("{}").split(",")])
    pal = np.array(pal)
    vals = text.split("SPR[64 * 64] = {")[1].split("};")[0]
    spr = np.array([int(v) for v in vals.replace("\n", " ").split(",") if v.strip()]).reshape(64, 64)
    return n, pal, spr


def paintable(spr, pal):
    """The C rule: goldish (R>60 and R>=B) or darkish (max < 0.14*255)."""
    r, g, b = pal[:, 0].astype(float), pal[:, 1].astype(float), pal[:, 2].astype(float)
    gold = (r > 60) & (r >= b)
    dark = np.maximum(np.maximum(r, g), b) < 0.14 * 255.0
    ok = gold | dark
    return ok[spr]  # 64x64 bool


def audit(face, only=None):
    sp = B.FACES[face]
    spr_n, pal, spr = load_sprite(os.path.join(AVATAR, sp["sprite"]))
    roles = sp["roles"](spr_n)
    ok = paintable(spr, pal)
    disc = np.array([[math.hypot(x - 31.5, y - 31.5) <= 29.0 for x in range(64)] for y in range(64)])
    print(f"== {face} (SPR_N={spr_n}) — paintable pixels inside disc: {int((ok & disc).sum())} of {int(disc.sum())}")
    rows = []
    for name, fn in B.MOODS:
        if only and name not in only:
            continue
        dur = B.DURS[name]
        n_frames = int(round(dur * 1000 / B.FRAME_MS))
        counts, blocked, eyebox = [], [], []
        for fi in range(n_frames):
            rt = fi * B.FRAME_MS / 1000.0
            f = B.Frame()
            fn(f, rt, 100.0 + rt, roles)
            pix = {p: c for p, c in f.pixels.items() if B.in_disc(p % 64, p // 64)}
            if name in B.EMBED:
                pix = {p: c for p, c in pix.items() if spr[p // 64, p % 64] != 0}
            surv = sum(1 for p in pix if ok[p // 64, p % 64])
            counts.append(surv)
            blocked.append(len(pix) - surv)
            # the runtime lens guard blocks a halo around the eyes (approx. x22..42, y26..35)
            eyebox.append(sum(1 for p in pix if 22 <= p % 64 <= 42 and 26 <= p // 64 <= 35))
        rows.append((name, n_frames, min(counts), sum(counts) / len(counts), max(counts), sum(blocked) / len(blocked), max(eyebox)))
    rows.sort(key=lambda r: r[3])
    print(f"{'mood':16} {'fr':>3} {'min':>4} {'mean':>6} {'max':>4} {'blocked/fr':>10} {'eyebox':>6}")
    for name, n, mn, mean, mx, bl, eb in rows:
        flag = "  <-- WEAK" if mean < 15 else ("  <-- DEAD-ISH" if mn == 0 and mean < 60 else "")
        ebf = "  EYE!" if eb > 4 else ""
        print(f"{name:16} {n:3} {mn:4} {mean:6.0f} {mx:4} {bl:10.0f} {eb:6}{flag}{ebf}")


if __name__ == "__main__":
    only = set(sys.argv[1:]) or None
    for face in ("golem", "stone"):
        audit(face, only)
        print()
