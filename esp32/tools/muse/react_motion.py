#!/usr/bin/env python3
"""Per-frame motion across a [react:] transition, for one face renderer.

Builds tools/muse/react_demo.c against the renderer, drives idle -> react -> idle,
and reports the changed-pixel count of every 40 ms frame around the reaction start
(and, with --dur, around the end). A cut shows as ONE huge spike; an eased transition
(T3.1 rig) spreads the same change over several frames.

    python3 react_motion.py <renderer.c> <react-name> [--fire 1.5] [--dur 2.8]
                            [--gif out.gif] [--window 1.2]

The GIF (when Pillow is present) covers [fire-0.2, fire+window] so the transition can
be watched, not only measured.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.expanduser("~/muse/muse-gadget-sdk/esp32")
DT = 0.04
MOVE = 30       # a pixel "changed" when the summed |RGB| delta exceeds this


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("react")
    ap.add_argument("--fire", type=float, default=1.5)
    ap.add_argument("--dur", type=float, default=0.0, help="reaction duration for the out-window")
    ap.add_argument("--gif", help="write the transition as a GIF here")
    ap.add_argument("--window", type=float, default=1.2, help="seconds after fire to report")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    import numpy as np
    from PIL import Image

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "rdemo")
        subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(ROOT, "components/muse"),
                        os.path.join(ROOT, "tools/muse/react_demo.c"), src, "-lm", "-o", exe],
                       check=True, capture_output=True, text=True)
        out = os.path.join(tmp, "frames")
        total = args.fire + max(args.dur, 0) + 1.2
        r = subprocess.run([exe, out, args.react, f"{args.fire}", f"{total}"],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
        files = sorted(os.listdir(out))
        px = [np.asarray(Image.open(os.path.join(out, f)).convert("RGB").resize((64, 64), Image.NEAREST)).astype(int)
              for f in files]
        mv = [int((np.abs(px[i] - px[i - 1]).sum(2) > MOVE).sum()) for i in range(1, len(px))]
        mv = [mv[0]] + mv            # align index i to frame i (frame 0 has no predecessor)

    fire_f = int(round(args.fire / DT))
    print(f"renderer: {src}")
    print(f"react: {args.react} fires at frame {fire_f} ({args.fire:.2f}s), {len(px)} frames")

    def window(lo_f, hi_f, label):
        print(f"\n{label}")
        print(f"{'frame':>5} {'t(s)':>6} {'changed':>8}")
        for i in range(max(0, lo_f), min(len(mv), hi_f)):
            print(f"{i:5d} {i * DT:6.2f} {mv[i]:8d}")

    def profile(ref_f, hi_f, label):
        """Distance from a reference frame: how fast the NEW expression settles. A cut
        reaches the final state in one frame; an eased transition builds over several."""
        ref = px[ref_f]
        conv = [int((np.abs(p - ref).sum(2) > MOVE).sum()) for p in px[ref_f:min(len(px), hi_f)]]
        print(f"\n{label} (pixels still different from frame {ref_f}):")
        print(f"{'frame':>5} {'t(s)':>6} {'diff':>8}")
        for i, c in enumerate(conv):
            print(f"{ref_f + i:5d} {(ref_f + i) * DT:6.2f} {c:8d}")

    window(fire_f - 2, fire_f + int(args.window / DT), "IN transition (fire):")
    profile(fire_f - 2, fire_f + int(args.window / DT),
            "IN convergence")
    if args.dur:
        end_f = fire_f + int(round(args.dur / DT))
        window(end_f - int(0.4 / DT), end_f + 10, "OUT transition (reaction ends):")
        profile(end_f - 4, end_f + 14, "OUT convergence")

    in_mv = mv[fire_f:fire_f + int(0.4 / DT)]
    print(f"\nIN window: max {max(in_mv)}, mean {np.mean(in_mv):.0f}, "
          f"frames over 25% of max: {sum(1 for v in in_mv if v > 0.25 * max(in_mv))}")

    if args.gif:
        lo = max(0, fire_f - int(0.2 / DT))
        hi = min(len(px), fire_f + int(args.window / DT))
        frames = [Image.fromarray(p.astype("uint8")).convert("P") for p in px[lo:hi]]
        frames[0].save(args.gif, save_all=True, append_images=frames[1:], duration=40, loop=0)
        print(f"gif: {args.gif} ({hi - lo} frames)")


if __name__ == "__main__":
    main()
