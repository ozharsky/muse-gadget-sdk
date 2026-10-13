#!/usr/bin/env python3
"""Measure animation *quality* of a Muse avatar renderer, not just that it runs.

The real defect class on this gadget is not crashes: it is DEAD FRAMES (consecutive
frames identical to the previous one) and BURSTY motion (a few frames slam the whole
screen while most do nothing). Meta's default Bloop renders every driver frame UNIQUELY;
a renderer that only writes 83 of 150 frames is 45% frozen and reads as a statue.

Usage:
    python3 qa_motion.py <renderer.c> [--ref <dir-of-gifs>] [--gate]

    <renderer.c>   a muse_pixel*.c (e.g. esp32/components/muse/avatar/muse_pixel_golem.c)
    --ref          a directory of reference .gif files to compare against (optional)
    --gate         exit non-zero if any animation has dead frames (for CI / pre-flash)

Reports per animation: frames, dead% (identical-to-previous), mean / median motion
(px changed per frame on the 64x64 grid), std/mean (smoothness; lower is better), max spike.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # <repo>/esp32
DT = 0.04
MOVE = 30        # a pixel "changed" when the summed |RGB| delta exceeds this


def build_and_dump(src, outdir):
    """cc anim.c against `src` (no symbol renames: standalone host build) and dump PPMs."""
    exe = os.path.join(outdir, "anim")
    cmd = ["cc", "-O2", "-w", "-I", os.path.join(ROOT, "components/muse"),
           os.path.join(ROOT, "tools/muse/anim.c"), src, "-lm", "-o", exe]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    frames_dir = os.path.join(outdir, "ppm")
    os.makedirs(frames_dir, exist_ok=True)
    subprocess.run([exe, frames_dir], check=True, capture_output=True, text=True)
    return frames_dir


def analyze(frames_dir):
    """Per-animation stats from the dumped PPMs."""
    import numpy as np
    from PIL import Image
    out = {}
    for sub in sorted(glob.glob(os.path.join(frames_dir, "*"))):
        name = os.path.basename(sub)
        files = sorted(glob.glob(os.path.join(sub, "*.ppm")))
        if not files:
            continue
        px = [np.asarray(Image.open(f).resize((64, 64), Image.NEAREST)).astype(int)
              for f in files]
        dead = sum(1 for i in range(1, len(px)) if np.abs(px[i] - px[i - 1]).sum() == 0)
        mv = [int((np.abs(px[i] - px[i - 1]).sum(2) > MOVE).sum()) for i in range(1, len(px))]
        mv = mv or [0]
        out[name] = {
            "frames": len(px),
            "dead": dead,
            "dead_pct": 100.0 * dead / max(len(px) - 1, 1),
            "mean": float(np.mean(mv)),
            "median": float(np.median(mv)),
            "std_over_mean": float(np.std(mv) / max(np.mean(mv), 1e-9)),
            "max": int(np.max(mv)),
        }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("--ref", help="directory of reference .gif files")
    ap.add_argument("--gate", action="store_true", help="exit 1 if any dead frames")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    with tempfile.TemporaryDirectory() as tmp:
        stats = analyze(build_and_dump(src, tmp))

    print(f"renderer: {src}")
    print(f"{'anim':12} {'frames':>6} {'dead%':>6} {'mean':>6} {'median':>7} {'std/mu':>7} {'max':>6}")
    bad = []
    for name, s in stats.items():
        flag = "  <-- DEAD" if s["dead"] else ""
        print(f"{name:12} {s['frames']:6} {s['dead_pct']:5.0f}% {s['mean']:6.0f} "
              f"{s['median']:7.0f} {s['std_over_mean']:7.2f} {s['max']:6}{flag}")
        if s["dead"]:
            bad.append(name)

    if args.ref and os.path.isdir(args.ref):
        import numpy as np
        from PIL import Image, ImageSequence
        print(f"\nreference: {args.ref}")
        for f in sorted(glob.glob(os.path.join(args.ref, "*.gif"))):
            name = os.path.basename(f)[:-4]
            if name not in stats:
                continue
            px = [np.asarray(fr.convert("RGB").resize((64, 64), Image.NEAREST)).astype(int)
                  for fr in ImageSequence.Iterator(Image.open(f))]
            mv = [int((np.abs(px[i] - px[i - 1]).sum(2) > MOVE).sum()) for i in range(1, len(px))]
            print(f"  {name:12} ref mean={np.mean(mv):6.0f} median={np.median(mv):6.0f} "
                  f"std/mu={np.std(mv)/max(np.mean(mv),1e-9):.2f}  "
                  f"(mine mean={stats[name]['mean']:.0f} std/mu={stats[name]['std_over_mean']:.2f})")

    if args.gate and bad:
        print(f"\nGATE FAIL: dead frames in {', '.join(bad)}", file=sys.stderr)
        sys.exit(1)
    print("\nOK" if not bad else f"\n{len(bad)} animation(s) with dead frames")


if __name__ == "__main__":
    main()
