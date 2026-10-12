#!/usr/bin/env python3
"""Contact sheets of every animation of a Muse face — SEE what the renderer draws.

    python3 tools/muse/preview_sheets.py --src components/muse/avatar/muse_pixel_golem.c \
            --out /tmp/sheets [--samples 8] [--max-rows 13] [--states] [--moods] [--only a,b,...]

Builds tools/muse/dump_anim.c against --src, dumps strided PPM frames for every
state and mood the face knows, and assembles labelled contact sheets (one PNG per
<= --max-rows rows; each row = one animation, --samples frames evenly sampled
across its duration). Prints the row order of every sheet, so a defect can be
referenced by sheet + row number.

This is the review loop for animation work: run it before/after a change and look
at the sheets (vision QA), instead of flashing and squinting at the glass.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # esp32/
STATE_NAMES = ["boot", "idle", "listening", "thinking", "speaking", "happy", "off", "error"]


def parse_moods(src_path):
    """[(name, seconds)] for every mood the face knows.

    Baked faces: the MOODS[] table of the header they include (frames * 167 ms).
    Procedural faces: their REACTIONS[R_COUNT] table (name, duration).
    """
    text = open(src_path).read()
    m = re.search(r'#include\s+"((?:golem|stone)_moods\.h)"', text)
    if m:
        hp = os.path.join(os.path.dirname(os.path.abspath(src_path)), m.group(1))
        h = open(hp).read()
        pairs = re.findall(r'\{\s*"([a-z_]+)",\s*(\d+),\s*(\d+)\s*\}', h)
        if pairs:
            return [(n, int(fr) * 0.167 + 0.5) for n, fr, _off in pairs]
    m = re.search(r'REACTIONS\[R_COUNT\]\s*=\s*\{(.*?)\n\};', text, re.S)
    if not m:
        raise SystemExit(f"preview_sheets: no moods header and no REACTIONS table in {src_path}")
    pairs = re.findall(r'\{\s*"([a-z_]+)",\s*([0-9.]+)f\s*\}', m.group(1))
    return [(n, float(d) + 0.5) for n, d in pairs]


def build(src, tmp):
    exe = os.path.join(tmp, "dump_anim")
    subprocess.run(["cc", "-O2", "-w", "-I", os.path.join(ROOT, "components/muse"),
                    os.path.join(ROOT, "tools/muse/dump_anim.c"), os.path.abspath(src),
                    "-lm", "-o", exe], check=True)
    return exe


def dump(exe, outdir, which, names, scale, samples):
    os.makedirs(outdir, exist_ok=True)
    r = subprocess.run([exe, outdir, str(scale), str(samples), which] + names,
                       capture_output=True, text=True)
    sys.stderr.write(r.stderr)
    if r.returncode:
        raise SystemExit(f"preview_sheets: dump failed ({which})")
    rows = []
    for n in names:
        base = n.split(":")[0]
        files = sorted(glob.glob(os.path.join(outdir, base, "*.ppm")))
        if not files:
            raise SystemExit(f"preview_sheets: no frames for {base}")
        rows.append((base, files))
    return rows


def sheet(face, kind, rows, out_dir, samples, tile=144, label_w=112, max_rows=13):
    paths = []
    for k in range(0, len(rows), max_rows):
        chunk = rows[k:k + max_rows]
        h = tile + 8
        img = Image.new("RGB", (label_w + samples * tile, len(chunk) * h + 18), (12, 12, 16))
        dr = ImageDraw.Draw(img)
        dr.text((4, 3), f"{face} / {kind} / sheet {k // max_rows + 1}", fill=(240, 240, 240))
        for r, (name, files) in enumerate(chunk):
            y = 16 + r * h
            dr.text((4, y + 4), f"{k + r + 1}:{name}", fill=(230, 230, 230))
            n = len(files)
            for i in range(samples):
                f = files[min(n - 1, round(i * (n - 1) / max(samples - 1, 1)))]
                im = Image.open(f).resize((tile, tile), Image.NEAREST)
                img.paste(im, (label_w + i * tile, y))
        p = os.path.join(out_dir, f"{face}_{kind}_{k // max_rows + 1}.png")
        img.save(p)
        paths.append((p, [nm for nm, _ in chunk]))
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--samples", type=int, default=8)
    ap.add_argument("--max-rows", type=int, default=13)
    ap.add_argument("--scale", type=int, default=4, help="render scale (PPM size = 64*scale)")
    ap.add_argument("--states", action="store_true")
    ap.add_argument("--moods", action="store_true")
    ap.add_argument("--only", help="comma-separated names to keep (moods and states)")
    args = ap.parse_args()

    if not (args.states or args.moods):
        args.states = args.moods = True
    face = os.path.basename(args.src).replace("muse_pixel_", "").replace(".c", "")
    os.makedirs(args.out, exist_ok=True)
    only = set(n.strip() for n in args.only.split(",")) if args.only else None

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(args.src, tmp)
        if args.states:
            st = [s for s in STATE_NAMES if not only or s in only]
            if st:
                rows = dump(exe, os.path.join(tmp, "st"), "state", st, args.scale, args.samples)
                for p, names in sheet(face, "states", rows, args.out, args.samples, max_rows=args.max_rows):
                    print(f"{p}")
                    print("   rows: " + "  ".join(f"{i + 1}:{n}" for i, n in enumerate(names)))
        if args.moods:
            moods = parse_moods(os.path.abspath(args.src))
            specs = [f"{n}:{d:.3f}" for n, d in moods if not only or n in only]
            if specs:
                rows = dump(exe, os.path.join(tmp, "mo"), "mood", specs, args.scale, args.samples)
                for p, names in sheet(face, "moods", rows, args.out, args.samples, max_rows=args.max_rows):
                    print(f"{p}")
                    print("   rows: " + "  ".join(f"{i + 1}:{n}" for i, n in enumerate(names)))
    print("preview_sheets: done")


if __name__ == "__main__":
    main()
