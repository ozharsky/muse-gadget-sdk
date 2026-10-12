#!/usr/bin/env python3
"""Idle-timeline analysis (T3.2 evidence): reads the PPM frames idle_demo.c dumped,
builds a full-face GIF + a zoomed eye-band GIF, and measures the resting rhythm
from the actual rendered pixels -- dead frames, per-frame motion, and the strong
events (blinks, doubles, saccades, micro beats) as runs of high-change frames.

    python3 tools/muse/idle_timeline.py FRAMEDIR --tag shodan [--out DIR] [--dump DIR]

Needs Pillow. Prints a compact stats block (also written to <out>/<tag>_stats.txt).
"""
from __future__ import annotations

import argparse
import os
import re
import sys

from PIL import Image, ImageChops

GRID = 64
CHANGE_LO = 8          # a pixel "changed" if any channel moved more than this
CHANGE_HI = 24         # "strongly changed" -- what an event is made of
EYE_BOX = (8, 20, 56, 46)   # grid coords: rows 20..46, cols 8..56 -- the eye band of all three faces


def load_frames(framedir: str):
    names = sorted(n for n in os.listdir(framedir) if n.endswith(".ppm"))
    names.sort(key=lambda n: int(re.search(r"(\d+)", n).group(1)))
    return [Image.open(os.path.join(framedir, n)).convert("RGB") for n in names]


def changed_count(a: Image.Image, b: Image.Image, box=None) -> int:
    """Pixels whose luma moved by more than CHANGE_HI (C-speed via PIL LUT+histogram)."""
    if box:
        a = a.crop(box)
        b = b.crop(box)
    d = ImageChops.difference(a.convert("L"), b.convert("L"))
    m = d.point(lambda v: 255 if v > CHANGE_HI else 0)
    return m.histogram()[255]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("framedir")
    ap.add_argument("--tag", default="face")
    ap.add_argument("--out", default=None)
    ap.add_argument("--dump", default=None, help="write top event frames as PNGs here")
    ap.add_argument("--top", type=int, default=8)
    args = ap.parse_args()

    frames = load_frames(args.framedir)
    if not frames:
        print("no frames")
        return 1
    n = len(frames)
    w = frames[0].width
    s = w // GRID
    eye = tuple(v * s for v in EYE_BOX)

    tot, eyeq = [], []
    prev = frames[0]
    for f in frames[1:]:
        tot.append(changed_count(prev, f))
        eyeq.append(changed_count(prev, f, box=eye))
        prev = f
    tot = [0] + tot
    eyeq = [0] + eyeq

    dead = sum(1 for v in tot if v < 8)
    ev_thr = max(10, sorted(tot)[int(len(tot) * 0.97)])   # the loudest 3% of frames
    events = []
    i = 1
    while i < n:
        if tot[i] >= ev_thr:
            j = i
            while j + 1 < n:
                k = j + 1
                if tot[k] >= ev_thr or (k + 1 < n and tot[k + 1] >= ev_thr) or (k + 2 < n and tot[k + 2] >= ev_thr):
                    j = k
                else:
                    break
            events.append((i, j, max(tot[i:j + 1]), sum(eyeq[i:j + 1])))
            i = j + 1
        else:
            i += 1

    # GIFs: full face at half of real time (keep every 2nd frame), eyes zoomed 2x.
    out = args.out or args.framedir
    os.makedirs(out, exist_ok=True)
    small = [f.resize((w // 2, w // 2), Image.NEAREST) for f in frames[::2]]
    small[0].save(os.path.join(out, f"{args.tag}_full.gif"), save_all=True,
                  append_images=small[1:], duration=80, loop=0)
    eyeframes = []
    ex0, ey0, ex1, ey1 = eye
    for f in frames:
        e = f.crop((ex0, ey0, ex1, ey1))
        eyeframes.append(e.resize((e.width * 2, e.height * 2), Image.NEAREST))
    eyeframes[0].save(os.path.join(out, f"{args.tag}_eyes.gif"), save_all=True,
                      append_images=eyeframes[1:], duration=80, loop=0)

    if args.dump:
        os.makedirs(args.dump, exist_ok=True)
        ranked = sorted(range(n), key=lambda i: -tot[i])[:args.top]
        for i in ranked:
            if tot[i] <= 0:
                continue
            f = frames[i].crop((ex0, ey0, ex1, ey1))
            f = f.resize((f.width * 3, f.height * 3), Image.NEAREST)
            f.save(os.path.join(args.dump, f"{args.tag}_f{i:04d}_chg{tot[i]}.png"))

    srt = sorted(tot)
    lines = [
        f"tag: {args.tag}  frames: {n}  seconds: {n * 0.04:.1f}  grid scale: {s}",
        f"dead frames (< 8 px): {dead} ({100.0 * dead / n:.1f}%)",
        f"changed px/frame: mean {sum(tot) / n:.1f}  median {srt[n // 2]}  p95 {srt[int(n * 0.95)]}  max {srt[-1]}",
        f"eye-band strong px/frame: mean {sum(eyeq) / n:.1f}  max {max(eyeq)}",
        f"events (top-3% frames, merged): {len(events)}",
    ]
    for (a, b, peak, e) in events[:20]:
        lines.append(f"  t={a * 0.04:6.2f}s..{b * 0.04:6.2f}s  dur={(b - a + 1) * 0.04:4.2f}s  peak {peak:5d}px  eye {e:5d}px")
    report = "\n".join(lines)
    print(report)
    with open(os.path.join(out, f"{args.tag}_stats.txt"), "w") as fh:
        fh.write(report + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
