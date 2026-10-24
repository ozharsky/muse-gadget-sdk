#!/usr/bin/env python3
"""AI portraits -> portrait packs in the gadget's asset partition (assets.bin, entry pt_<name>).

    python3 tools/muse/portrait_pack.py SRC_DIR NAME [--preview out.png] [--no-assets]

SRC_DIR holds 1024 px renders on black, all with the same framing:
  base.png                  the face at rest
  <variant>.png             any of VARIANTS below (photo faces): the same face, one thing changed
  face.png                  screen faces only: base.png is the blank screen, face.png the same
                            robot with its face lit (the device draws the face itself)
Each variant becomes a patch: only the pixels that differ from base.png (a soft mask, the main
blobs inside the feature's box) are kept, so generation drift elsewhere never reaches the glass.
The device blends patches by weight (avatar/muse_portrait.c, whose pack_hdr_t is HDR here).
The pack is written to portraits/portrait_<name>.bin and replaced in assets.bin, which
build_flash.sh writes to the assets partition (no app rebuild needed).
"""
import io
import os
import struct
import subprocess
import sys
from collections import deque

import numpy as np
from PIL import Image, ImageFilter

SIZE = 384                  # the face canvas on the 1.75C (64 cells x 6 px)
EDGE_FADE = 28              # px over which the picture fades to black at the canvas edge
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_DIR = os.path.join(ROOT, "portraits")
EYES, MOUTH = (0.15, 0.85, 0.22, 0.58), (0.25, 0.75, 0.45, 0.82)   # search boxes x0, x1, y0, y1
FACE = (0.15, 0.85, 0.22, 0.82)
# Tag = index (enum in muse_portrait.c). The device applies patches in this order, so the blink
# lands over a glance and the talking mouth over a smile.
VARIANTS = (("eyes_half", EYES, 2), ("eyes_left", EYES, 2), ("eyes_right", EYES, 2), ("eyes_up", EYES, 2),
            ("eyes_down", EYES, 2), ("eyes_wide", EYES, 2), ("eyes_angry", EYES, 2), ("brow_raise", EYES, 1),
            ("wink", EYES, 1), ("eyes_glow", EYES, 2), ("eyes_closed", EYES, 2), ("sad", FACE, 3), ("laugh", FACE, 3),
            ("smile", MOUTH, 1), ("smirk", MOUTH, 1), ("mouth_1", MOUTH, 1), ("mouth_2", MOUTH, 1), ("mouth_3", MOUTH, 1))
MAX_PATCHES = 20
HDR = "<4sHBBII" + "4hhhhhhIhhhh" + "BB4HII" * MAX_PATCHES + "III"   # ... light_off, phase_off, light colour
assert struct.calcsize(HDR) == 418, "pack header: keep in step with pack_hdr_t in muse_portrait.c"


def prep(path, black=None):
    """1024 px render -> SIZE px on true black. black: the backdrop level (from base.png's border)."""
    im = Image.open(path).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32)
    if black is None:
        corners = np.concatenate([a[:24, :24], a[:24, -24:], a[-24:, :24], a[-24:, -24:]]).reshape(-1, 3)
        black = float(np.clip(np.median(corners.max(axis=1)) + 4, 8, 45))   # corners: backdrop, never the subject
    a = np.clip((a - black) * 255.0 / (255.0 - black), 0, 255)   # true black for the AMOLED
    r = np.arange(SIZE, dtype=np.float32)
    edge = np.minimum(np.minimum(r, SIZE - 1 - r) / EDGE_FADE, 1.0)
    a *= (np.minimum.outer(edge, edge) ** 1.5)[..., None]        # no square outline on the glass
    return a, black


def rgb565(a):
    a = a.astype(np.uint16)
    return ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)


def soft_mask(base, variants, box, blobs):
    diff = np.max([np.abs(v - base).mean(axis=2) for v in variants], axis=0)
    m = np.clip((diff - 10.0) / 25.0, 0, 1)
    x0, x1, y0, y1 = (int(f * SIZE) for f in box)
    outside = m.copy()
    outside[y0:y1, x0:x1] = 0
    keep = np.zeros_like(m)
    keep[y0:y1, x0:x1] = m[y0:y1, x0:x1]
    im = Image.fromarray((keep * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(9))
    core = np.asarray(im) > 60
    main = np.zeros_like(core)
    for _, (bx0, by0, bx1, by1) in components(core)[:blobs]:   # the feature itself, not specks of texture
        main[by0:by1 + 1, bx0:bx1 + 1] |= core[by0:by1 + 1, bx0:bx1 + 1]
    im = Image.fromarray((main * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(4))
    m = np.clip(np.asarray(im).astype(np.float32) / 255.0 * 1.6, 0, 1)
    drift = float((outside > 0.5).mean())
    ys, xs = np.nonzero(m > 0.03)
    if not len(xs):
        return None, drift
    return (m, (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)), drift


def components(mask):
    """4-connected blobs of a bool mask, biggest first: (area, (x0, y0, x1, y1))."""
    seen = np.zeros_like(mask)
    out = []
    for y, x in zip(*np.nonzero(mask)):
        if seen[y, x]:
            continue
        q, n, bb = deque([(y, x)]), 0, [x, y, x, y]
        seen[y, x] = True
        while q:
            cy, cx = q.popleft()
            n += 1
            bb = [min(bb[0], cx), min(bb[1], cy), max(bb[2], cx), max(bb[3], cy)]
            for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                if 0 <= ny < SIZE and 0 <= nx < SIZE and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    q.append((ny, nx))
        out.append((n, tuple(bb)))
    return sorted(out, reverse=True)


def screen_face(base, face):
    """Where the lit face sits on the blank screen: eyes, brows, mouth, glass, glow colour."""
    glow = (face - base).clip(0)
    lit = glow.mean(axis=2) > 40
    blobs = [b for a, b in components(lit) if a > 30]
    eyes = sorted(sorted(blobs, key=lambda b: -(b[2] - b[0]) * (b[3] - b[1]))[:2])
    if len(eyes) < 2:
        sys.exit("screen face: could not find two eyes")
    (l, r) = [((b[0] + b[2]) // 2, (b[1] + b[3]) // 2, (b[2] - b[0]) // 2) for b in eyes]
    ey = max(l[1], r[1])
    below = [b for b in blobs if b[1] > ey + l[2] // 2 and b[0] < r[0] and b[2] > l[0]]
    above = [b for b in blobs if b[3] < min(l[1], r[1]) - l[2] // 2]
    mouth = max(below, key=lambda b: b[2] - b[0]) if below else (l[0], ey + 2 * l[2], r[0], ey + 2 * l[2])
    brow_dy = (min(l[1], r[1]) - (above[0][1] + above[0][3]) // 2) if above else int(l[2] * 1.6)
    # where the glow may reach: the face's own proportions (the backdrop is as dark as the glass)
    rad = (l[2] + r[2]) // 2
    g = (l[0] - 3 * rad, min(l[1], r[1]) - brow_dy - 2 * rad, r[0] + 3 * rad, mouth[3] + 2 * rad)
    hot = glow[lit]
    col = np.median(hot[hot.mean(axis=1) > np.percentile(hot.mean(axis=1), 70)], axis=0)
    col = col * (255.0 / max(col.max(), 1))
    return dict(eye_l=l[:2], eye_r=r[:2], eye_rad=int((l[2] + r[2]) / 2), brow_dy=int(brow_dy),
                mouth=((mouth[0] + mouth[2]) // 2, (mouth[1] + mouth[3]) // 2), mouth_w=int(mouth[2] - mouth[0]),
                glow=(int(col[0]) << 16) | (int(col[1]) << 8) | int(col[2]), clip=g)


def light_maps(base, eyes):
    """Photo faces: which pixels are the face's own light (its glowing eyes, cables, seams), and how
    far each lies from the eyes (waves of light travel outwards along them). The light map is full
    size, the distance half size; both 0..255. Also the light's colour."""
    hsv = np.asarray(Image.fromarray(base.clip(0, 255).astype(np.uint8)).convert("HSV")).astype(np.float32) / 255.0
    hue, sat, val = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    lit = (sat > 0.45) & (val > 0.45)
    ang = hue[lit] * 2 * np.pi   # the accent: the circular mean hue of the lit pixels, weighted by brightness
    w = val[lit]
    accent = (np.arctan2((np.sin(ang) * w).sum(), (np.cos(ang) * w).sum()) / (2 * np.pi)) % 1.0
    dh = np.abs(hue - accent)
    dh = np.minimum(dh, 1 - dh)
    light = np.clip((sat - 0.25) / 0.3, 0, 1) * np.clip((val - 0.32) / 0.35, 0, 1) * np.clip(1 - dh / 0.1, 0, 1)
    col = np.median(base[light > 0.6], axis=0) if (light > 0.6).any() else np.array([80, 255, 80])
    ex0, ey0, ex1, ey1 = eyes[2]   # the eyes' bright cores glow too, whatever their colour (often near white)
    core = np.zeros_like(light)
    core[ey0:ey1, ex0:ex1] = np.clip((val[ey0:ey1, ex0:ex1] - 0.5) / 0.3, 0, 1)
    light = np.maximum(light, core)
    col = col * (255.0 / max(col.max(), 1))
    yy, xx = np.mgrid[0:SIZE:2, 0:SIZE:2].astype(np.float32)
    d = np.hypot(xx - eyes[0], yy - eyes[1])
    reach = np.percentile(d[light[::2, ::2] > 0.3], 98) if (light[::2, ::2] > 0.3).any() else SIZE * 0.7
    phase = np.clip(d / max(reach, 1) * 255, 0, 255)
    print(f"  light: {(light > 0.3).mean():.1%} of the picture, colour #{int(col[0]):02x}{int(col[1]):02x}{int(col[2]):02x}")
    return (light * 255).astype(np.uint8), phase.astype(np.uint8), (int(col[0]) << 16) | (int(col[1]) << 8) | int(col[2])


def main():
    src, name = sys.argv[1], sys.argv[2]
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    base, black = prep(os.path.join(src, "base.png"))
    buf = io.BytesIO()
    Image.fromarray(base.astype(np.uint8)).save(buf, "JPEG", quality=92, subsampling=0)
    jpg = buf.getvalue()

    data = bytearray()
    off = struct.calcsize(HDR)
    off += -off % 4

    def put(b):
        nonlocal data
        pos = off + len(data)
        data += b + b"\0" * (-len(b) % 4)
        return pos

    jpg_off = put(jpg)
    kind, scr, patches, tiles = 0, None, [], [base]
    if os.path.exists(os.path.join(src, "face.png")):
        kind, scr = 1, screen_face(base, prep(os.path.join(src, "face.png"), black)[0])
        print(name, "screen face:", scr)
    for tag, (vname, box, blobs) in enumerate(VARIANTS):
        path = os.path.join(src, vname + ".png")
        if not os.path.exists(path):
            continue
        v = prep(path, black)[0]
        res, drift = soft_mask(base, [v], box, blobs)
        if not res:
            print(f"{name} {vname}: no change found, skipped")
            continue
        m, (x0, y0, x1, y1) = res
        print(f"{name} {vname}: {x0},{y0} {x1 - x0}x{y1 - y0}" + (f"  (drift outside {drift:.1%})" if drift > 0.02 else ""))
        mask_off = put((m[y0:y1, x0:x1] * 255).astype(np.uint8).tobytes())
        var_off = put(rgb565(v[y0:y1, x0:x1]).astype("<u2").tobytes())
        patches.append((tag, 0, x0, y0, x1 - x0, y1 - y0, mask_off, var_off))
        t = base.copy()
        t[y0:y1, x0:x1] += (v - base)[y0:y1, x0:x1] * m[y0:y1, x0:x1, None]
        tiles.append(t)
    if len(patches) > MAX_PATCHES:
        sys.exit("too many variants")
    light_off = phase_off = light_col = 0
    if kind == 0:
        closed = [p for p in patches if VARIANTS[p[0]][0] == "eyes_closed"]
        c = closed[0] if closed else (0, 0, SIZE // 3, SIZE // 3, SIZE // 3, SIZE // 6)
        eyes = (c[2] + c[4] / 2, c[3] + c[5] / 2, (c[2], c[3], c[2] + c[4], c[3] + c[5]))
        light, phase, light_col = light_maps(base, eyes)
        light_off, phase_off = put(light.tobytes()), put(phase.tobytes())

    flat = [x for p in patches for x in p] + [0] * 8 * (MAX_PATCHES - len(patches))
    s = scr or dict(eye_l=(0, 0), eye_r=(0, 0), eye_rad=0, brow_dy=0, mouth=(0, 0), mouth_w=0, glow=0, clip=(0, 0, 0, 0))
    hdr = struct.pack(HDR, b"MPT1", SIZE, kind, len(patches), jpg_off, len(jpg),
                      *s["eye_l"], *s["eye_r"], s["eye_rad"], s["brow_dy"], *s["mouth"], s["mouth_w"], s["glow"], *s["clip"],
                      *flat, light_off, phase_off, light_col)
    blob = hdr + b"\0" * (-len(hdr) % 4) + bytes(data)
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, f"portrait_{name}.bin")
    open(out, "wb").write(blob)
    print(f"{name}: {len(blob) // 1024} KB ({len(jpg) // 1024} KB base JPEG, {len(patches)} patches) -> {os.path.relpath(out, ROOT)}")
    if "--no-assets" not in sys.argv:
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "muse", "assets_pack.py"), "--from", out,
                        "--name", f"pt_{name}", "--fmt", "blob", "--append", "--out", os.path.join(ROOT, "assets.bin")], check=True)
    if preview:
        cols = 5
        tiles += [np.zeros_like(base)] * (-len(tiles) % cols)
        rows = [np.hstack(tiles[i:i + cols]) for i in range(0, len(tiles), cols)]
        Image.fromarray(np.vstack(rows).clip(0, 255).astype(np.uint8)).save(preview)


if __name__ == "__main__":
    main()
