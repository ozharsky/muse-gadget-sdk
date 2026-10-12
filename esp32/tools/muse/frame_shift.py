#!/usr/bin/env python3
"""Measure per-frame ROW TELEPORTS in a dumped animation (the error-crack defect class).

The faces' ERROR state slides thin row bands sideways. When the bands were redrawn
from scratch every frame, each row's shift jumped by up to the full band range between
consecutive frames -- the "cracks teleport" complaint. This tool quantifies exactly
that from a directory of PPM frames (e.g. an `anim.c` dump's error/ subdir):

  for each consecutive frame pair, for each row:
    best = argmax over sh in [-max .. max] of #(x: A[y][x] == B[y][x+sh])
    jump[y] = |best_this_pair[y] - best_prev_pair[y]|      (rows whose content changed
                                                            too much to match are skipped)

Reports: worst/mean rows jumping >= 2 px per frame (a teleport), the max jump, and per
row the longest run of frames with a nonzero shift (tear persistence: an eased tear
holds its row for many frames, a teleporting band only for one or two).

    python3 tools/muse/frame_shift.py <dir-of-ppm-frames> [--skip 25] [--max-shift 6]

--skip drops the leading frames (the ERROR state opens with a deliberate whole-frame
shake for its first 0.6 s; that is intentional, not a crack teleport).
"""
import argparse
import glob
import os
import re
import sys

import numpy as np

S = 5           # anim.c dumps at 5 px per grid cell (MUSE_PX_W * 5)
W = 64


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        raise SystemExit(f"{path}: not a binary P6 PPM")
    w, h = int(m.group(1)), int(m.group(2))
    px = np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=m.end())
    return px.reshape(h, w, 3)


def grid(img):
    """Sample the center pixel of every 5x5 cell (edge cells are dimmed by the scale map)."""
    return img[2::S, 2::S]      # (64, 64, 3); cell centers never carry the edge-dim bit


def row_shift(a, b, max_shift):
    """Per row: the horizontal slide of a->b (b[x+sh] == a[x]), or None if unmatched."""
    out = []
    for y in range(W):
        ra, rb = a[y], b[y]
        row = []
        scores = []
        for sh in range(-max_shift, max_shift + 1):
            if sh >= 0:
                xa = np.arange(0, W - sh)
                xb = xa + sh
            else:
                xa = np.arange(-sh, W)
                xb = xa + sh
            same = np.all(ra[xa] == rb[xb], axis=-1)
            scores.append((int(same.sum()), sh))
        best, sh = max(scores)
        if best < int(W * 0.82):
            out.append(None)
        elif sh != 0 and [s for s, x in scores if x == 0][0] == best:
            out.append(0)          # tie: prefer the no-shift reading
        else:
            out.append(sh)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--skip", type=int, default=25)
    ap.add_argument("--max-shift", type=int, default=6)
    args = ap.parse_args()

    frames = sorted(glob.glob(os.path.join(args.dump, "*.ppm")))
    if len(frames) < 3:
        raise SystemExit(f"{args.dump}: fewer than 3 PPM frames found")
    imgs = [grid(read_ppm(p)) for p in frames]

    abs_shift = [0] * W          # integrated shift per row (delta of deltas summed)
    runs = [0] * W
    max_runs = [0] * W
    moved = [0] * W
    jumps_ge2 = []
    max_jump = 0
    total_jump = 0
    n_jump = 0
    unknown = 0

    for i in range(args.skip + 1, len(frames)):
        cur = row_shift(imgs[i - 1], imgs[i], args.max_shift)   # per-row delta A->B
        pair_jumps = 0
        for y in range(W):
            if cur[y] is None:
                unknown += 1
                continue
            d = abs(cur[y])                                     # the row's jump THIS frame
            if d > max_jump:
                max_jump = d
            if d >= 2:
                pair_jumps += 1
            total_jump += d
            n_jump += 1
            abs_shift[y] += cur[y]
            if abs_shift[y] != 0:
                moved[y] = 1
                runs[y] += 1
                max_runs[y] = max(max_runs[y], runs[y])
            else:
                runs[y] = 0
        jumps_ge2.append(pair_jumps)

    worst = max(jumps_ge2) if jumps_ge2 else 0
    mean = sum(jumps_ge2) / len(jumps_ge2) if jumps_ge2 else 0.0
    longest = max(max_runs) if max_runs else 0
    moved_n = sum(moved)
    print(f"{args.dump}")
    print(f"  frames={len(frames)} analyzed={len(frames)-args.skip-1} unknown-row-cells={unknown}")
    print(f"  row jumps >=2 px/frame: worst={worst} mean={mean:.2f}  max jump={max_jump} px  (mean |jump|={total_jump/max(n_jump,1):.2f})")
    print(f"  tear persistence: longest nonzero-shift run={longest} frames ({longest*0.04:.2f} s), rows moved={moved_n}/{W}")
    ok = max_jump <= 1 and worst == 0 and longest >= 13
    print("  VERDICT:", "OK (no teleports, tears persist)" if ok else "TELEPORTS PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
