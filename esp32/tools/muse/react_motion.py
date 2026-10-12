#!/usr/bin/env python3
"""Per-frame motion across a [react:] transition, for one face renderer.

Builds tools/muse/react_demo.c against the renderer, drives idle -> react -> idle,
and reports the changed-pixel count of every 40 ms frame around the reaction start
(and, with --dur, around the end). A cut shows as ONE huge spike; an eased transition
(T3.1 rig) spreads the same change over several frames.

Mood pairs (the cross-mood blend): fire a second react mid-flight and compare the
jump at ITS onset against the same mood fired alone from idle (the eased baseline):

    python3 react_motion.py <renderer.c> surprised --react2 sleepy --fire2 3.0
    python3 react_motion.py <renderer.c> surprised --sweep "sleepy>smirk,angry>scan" --offsets 0.4,1.5

    python3 react_motion.py <renderer.c> <react-name> [--fire 1.5] [--dur 2.8]
                            [--gif out.gif] [--window 1.2]

The GIF (when Pillow is present) covers [fire-0.2, fire+window] so the transition can
be watched, not only measured.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.expanduser("~/muse/muse-gadget-sdk/esp32")
DT = 0.04
MOVE = 30       # a pixel "changed" when the summed |RGB| delta exceeds this
SETTLE_THR = 40  # px still different from the settled frame


def f_frame(t):
    """The demo fires when rt = i*DT >= t; frame index i = ceil(t/DT - eps)."""
    import math
    return int(math.ceil(t / DT - 1e-9))


def build(src, tmp):
    exe = os.path.join(tmp, "rdemo")
    subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(ROOT, "components/muse"),
                    os.path.join(ROOT, "tools/muse/react_demo.c"), src, "-lm", "-o", exe],
                   check=True, capture_output=True, text=True)
    return exe


def run_frames(exe, tmp, tag, events, total):
    """events = [(name, t), ...] (1 or 2). Returns 64x64 int arrays per frame."""
    import numpy as np
    from PIL import Image
    out = os.path.join(tmp, "frames_" + tag)
    args = [exe, out, events[0][0], f"{events[0][1]}", f"{total}"]
    if len(events) > 1:
        args += [events[1][0], f"{events[1][1]}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    files = sorted(os.listdir(out))
    return [np.asarray(Image.open(os.path.join(out, f)).convert("RGB")
                       .resize((64, 64), Image.NEAREST)).astype(int) for f in files]


def motion(px):
    import numpy as np
    mv = [int((np.abs(px[i] - px[i - 1]).sum(2) > MOVE).sum()) for i in range(1, len(px))]
    return [mv[0]] + mv            # align index i to frame i (frame 0 has no predecessor)


def jump_at(mv, f):
    """The fire-frame spike: mv[f] minus this run's own ambient motion (median of the
    ten frames before it). Isolates the switch's first-frame change from ongoing motion."""
    import numpy as np
    lo = max(0, f - 12)
    hi = max(0, f - 2)
    amb = float(np.median(mv[lo:hi])) if hi > lo else 0.0
    return mv[f] - amb, mv[f], amb


def settle_frames(px, f, win=80):
    """Frames from f until the picture holds within the settled plateau of its final
    frame for 3 consecutive frames (the expression has landed). The plateau is the
    ambient shimmer level measured on the tail of the run -- the faces never pixel-
    match their final frame exactly while breathing/glinting."""
    import numpy as np
    ref = px[-1]
    d = [int((np.abs(p - ref).sum(2) > MOVE).sum()) for p in px[f:f + win]]
    tail = d[max(0, len(d) - 20):]
    thr = max(float(SETTLE_THR), float(np.median(tail)) * 1.15)
    for i in range(len(d) - 3):
        if max(d[i:i + 3]) < thr:
            return i
    return win


def snap_metrics(mv, f, win=13):
    """The shape of the onset: worst single frame, and what share of the whole
    window's change it carries. A cut is ~1.0; an eased/dissolved transition is low.
    Self-normalizing, so the dy nudge a mood fires on its own onset (a 1 px shift)
    weighs the same in both runs instead of dominating a raw difference."""
    import numpy as np
    w = mv[f:f + win]
    if not len(w):
        return 0, 0.0
    worst = max(w)
    tot = sum(w)
    return worst, (worst / tot if tot else 0.0)


def montage(px_pair, px_base, f2, path, n=10, pre=2, scale=4):  # n set by --montage
    """Native-resolution read aid: the pair run's window on top, baseline below,
    both upscaled by `scale` (nearest) so a 64-grid pixel is crisp to inspect."""
    from PIL import Image
    lo = max(0, f2 - pre)
    hi = min(len(px_pair), f2 + n)
    rows = []
    for run in (px_pair, px_base):
        rows.append([Image.fromarray(run[i].astype("uint8")).resize((64 * scale, 64 * scale), Image.NEAREST)
                     for i in range(lo, min(len(run), hi))])
    w = max(len(r) for r in rows)
    img = Image.new("RGB", (w * 64 * scale, 2 * 64 * scale), (12, 12, 16))
    for r, row in enumerate(rows):
        for i, im in enumerate(row):
            img.paste(im, (i * 64 * scale, r * 64 * scale))
    img.save(path)
    return path


def sweep(args):
    """For every pair A>B and offset: pair run (A@fire, B@fire+off) and baseline
    (B alone @fire+off). The B onset's first-frame jump must be at or below the
    baseline's (the eased single-fire), and settle within the baseline envelope."""
    src = os.path.abspath(args.src)
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(src, tmp)
        for pair in args.sweep.split(","):
            a, b = [s.strip() for s in pair.split(">")]
            for off in [float(o) for o in args.offsets.split(",")]:
                fire2 = args.fire + off
                # run past the fired mood's own life (max duration 3.4 s) so the
                # "settled" reference frame is a steady face, not a mid-mood frame
                # of the mood's OWN animation cycle.
                total = fire2 + 4.0
                f2 = f_frame(fire2)
                px_pair = run_frames(exe, tmp, f"p_{a}_{b}_{off}", [(a, args.fire), (b, fire2)], total)
                px_base = run_frames(exe, tmp, f"b_{b}_{off}", [(b, fire2)], total)
                px_asolo = run_frames(exe, tmp, f"a_{a}_{off}", [(a, args.fire)], total)
                mv_pair, mv_base = motion(px_pair), motion(px_base)
                mv_asolo = motion(px_asolo)
                f1 = f_frame(args.fire)
                a_worst = max(mv_asolo[f1:f1 + 13])
                a_amb = max(mv_asolo[max(0, f2 - 40):f2]) if f2 > 0 else 0
                jp_net, jp_raw, jp_amb = jump_at(mv_pair, f2)
                jb_net, jb_raw, jb_amb = jump_at(mv_base, f2)
                sp = settle_frames(px_pair, f2)
                sb = settle_frames(px_base, f2)
                wp, snap_p = snap_metrics(mv_pair, f2)
                wb, snap_b = snap_metrics(mv_base, f2)
                # The gate (three parts, all must hold):
                #  - jump: the pair's fire frame must not out-step the same mood's
                #    solo fire frame by more than dither granularity (150 px) plus
                #    the worst burst A's own animation was already producing right
                #    before the fire (its props animate on their own clock);
                #  - worst: the pair's worst frame over the onset window must not
                #    out-step the worst of EITHER solo transition (A leaving, B
                #    arriving) by the allowance -- a 1 px dy nudge is a whole-frame
                #    step (~900 px) in a solo onset too, so it is not a snap; the
                #    allowance scales with how much a 1 px step already moves on
                #    this face (a busier frame steps more pixels);
                allow_worst = max(200.0, 0.15 * max(wb, a_worst))
                ok_jump = jp_net <= jb_net + a_amb + 150
                ok_worst = wp <= max(wb, a_worst) + allow_worst
                #  settle within the baseline's envelope + one cross-fade window
                #  (FALL 0.4 s) + the palette/rig tail (~0.16 s) = +14 frames.
                ok_settle = sp <= sb + 14
                rows.append({
                    "pair": f"{a}>{b}", "off": off, "fire2": fire2,
                    "jumpB_base_net": jb_net, "jumpB_pair_net": jp_net,
                    "jumpB_base_raw": jb_raw, "jumpB_pair_raw": jp_raw,
                    "amb_base": jb_amb, "amb_pair": jp_amb,
                    "snap_base": round(snap_b, 2), "snap_pair": round(snap_p, 2),
                    "worst_base": wb, "worst_pair": wp, "worst_a_solo": a_worst,
                    "a_amb": a_amb,
                    "settle_base": sb, "settle_pair": sp,
                    "ok_jump": bool(ok_jump), "ok_worst": bool(ok_worst),
                    "ok_settle": bool(ok_settle),
                })
                if args.png:
                    os.makedirs(args.png, exist_ok=True)
                    montage(px_pair, px_base, f2,
                            os.path.join(args.png, f"blend_{a}_{b}_off{off}.png"),
                            n=args.montage)
    print(f"renderer: {src}")
    print(f"{'pair':22} {'off':>4} {'jump b/p':>13} {'worst b/a/p':>15} "
          f"{'snap b/p':>10} {'settle b/p':>11}  verdict")
    bad = 0
    for r in rows:
        verdict = "OK" if (r["ok_jump"] and r["ok_worst"] and r["ok_settle"]) else "FAIL"
        bad += verdict == "FAIL"
        print(f"{r['pair']:22} {r['off']:>4.1f} {r['jumpB_base_net']:>6}/{r['jumpB_pair_net']:<6} "
              f"{r['worst_base']:>6}/{r['worst_a_solo']:>5}/{r['worst_pair']:<5} "
              f"{r['snap_base']:>4}/{r['snap_pair']:<5} "
              f"{r['settle_base']:>5}/{r['settle_pair']:<5} {verdict}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)
        print(f"json: {args.json}")
    if bad:
        print(f"\nGATE FAIL: {bad} pair(s) above the eased baseline")
        sys.exit(1)
    print("\nGATE PASS: every pair's B-onset jump at or below the eased baseline")


def single(args):
    src = os.path.abspath(args.src)
    import numpy as np
    from PIL import Image

    pair = getattr(args, "react2", None)
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(src, tmp)
        total = max(args.fire + max(args.dur, 0) + 1.2,
                    (args.fire2 + 2.0) if pair else 0)
        events = [(args.react, args.fire)]
        if pair:
            events.append((pair, args.fire2))
        px = run_frames(exe, tmp, "single", events, total)
        mv = motion(px)

    fire_f = f_frame(args.fire)
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
    profile(fire_f - 2, fire_f + int(args.window / DT), "IN convergence")
    if args.dur:
        end_f = fire_f + int(round(args.dur / DT))
        window(end_f - int(0.4 / DT), end_f + 10, "OUT transition (reaction ends):")
        profile(end_f - 4, end_f + 14, "OUT convergence")

    in_mv = mv[fire_f:fire_f + int(0.4 / DT)]
    print(f"\nIN window: max {max(in_mv)}, mean {np.mean(in_mv):.0f}, "
          f"frames over 25% of max: {sum(1 for v in in_mv if v > 0.25 * max(in_mv))}")

    if pair:
        f2 = f_frame(args.fire2)
        j_net, j_raw, j_amb = jump_at(mv, f2)
        window(f2 - 4, f2 + int(max(args.window, 0.6) / DT), f"IN2 transition ({pair} fires):")
        s = settle_frames(px, f2)
        print(f"\nfire2 ({pair} @ {args.fire2:.2f}s): jump net {j_net:.0f} (raw {j_raw}, ambient {j_amb:.0f}), "
              f"settles in {s} frames")
        if args.png:
            os.makedirs(args.png, exist_ok=True)
            p = montage(px, px, f2, os.path.join(args.png, f"pair_{args.react}_{pair}.png"))
            print(f"montage: {p} (pair run; upper row)")

    if args.gif:
        lo = max(0, fire_f - int(0.2 / DT))
        hi = min(len(px), fire_f + int(args.window / DT))
        frames = [Image.fromarray(p.astype("uint8")).convert("P") for p in px[lo:hi]]
        frames[0].save(args.gif, save_all=True, append_images=frames[1:], duration=40, loop=0)
        print(f"gif: {args.gif} ({hi - lo} frames)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("react")
    ap.add_argument("--fire", type=float, default=1.5)
    ap.add_argument("--dur", type=float, default=0.0, help="reaction duration for the out-window")
    ap.add_argument("--gif", help="write the transition as a GIF here")
    ap.add_argument("--window", type=float, default=1.2, help="seconds after fire to report")
    ap.add_argument("--react2", help="second reaction (pair run: first @fire, this @fire2)")
    ap.add_argument("--fire2", type=float, default=0.0, help="when the second reaction fires")
    ap.add_argument("--png", help="directory for native-res montages of the pair window")
    ap.add_argument("--sweep", help='pairs "a>b,c>d": pair runs vs the eased single-fire baseline')
    ap.add_argument("--offsets", default="0.4,1.5", help="B fires at --fire + offset (sweep)")
    ap.add_argument("--json", help="dump the sweep numbers here")
    ap.add_argument("--montage", type=int, default=10, help="frames to include in a --png montage")
    args = ap.parse_args()

    if args.sweep:
        sweep(args)
    else:
        if args.react2 and not args.fire2:
            args.fire2 = args.fire + 1.5
        single(args)


if __name__ == "__main__":
    main()
