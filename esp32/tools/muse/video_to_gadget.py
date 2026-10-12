#!/usr/bin/env python3
"""Video -> gadget avatar animation (v2: indexed frames).

WHAT IT DOES
  1. ffmpeg extracts frames from any video (mp4/mov/webm/gif...) at a chosen fps.
  2. Each frame is snapped to a clip-wide palette at 64x64 (the gadget grid) --
     chunky pixel art, so the nearest-neighbour ~6x blow-up looks right.
  3. Frames are written as a header. Three formats:
       rgb565  legacy: full frames, uint16 per pixel          -> 8192 B/frame
       u8      indexed: palette + one byte per pixel          -> 4096 B/frame (2.0x)
       i4      indexed: palette + packed 4-bit (<=16 colours) -> (2048 B + pad)/frame (4.0x)
     The indexed formats emit the palette SEPARATELY, so a later palette swap
     recolours a clip for free. This mirrors the upstream muse-gadget-sdk's
     happy_anim toolchain (uint8 palette indices + RGB565 palette).

PALETTE DISCIPLINE (why clips don't flicker)
  One clip-wide palette, built once over ALL frames (median cut). Dithering is
  NONE (default; majority-per-cell, crispest) or BAYER (ordered 4x4, the same
  matrix the firmware itself uses) -- never error diffusion, which is
  frame-incoherent by construction and makes static areas shimmer.

BEST QUALITY THAT FITS (--fit)
  --fit KB searches fps (a --fps divisor) keeping the highest that fits the data
  budget, and records the search table in the manifest.json. If even the lowest
  fps does not fit, the clip is truncated to the budget (recorded).

I4 PACKING (our convention, fixed by the device player)
  Two pixels per byte, row-major, even-x pixel in the LOW nibble, odd-x in the
  high nibble: byte = idx[2j] | (idx[2j+1] << 4). 64 px = 32 bytes per row.
  Values are palette indices 0..npal-1 (npal <= 16).

USAGE
  python3 video_to_gadget.py <video> [--fps 12] [--secs N] [--start S] [--colors 24]
        [--format rgb565|u8|i4] [--dither none|bayer] [--fit KB]
        [--name NAME] [--out out/name] [--gif preview.gif] [--header name.h]

Outputs (into --out dir):
  <name>.h      the frame header (see formats above)
  preview.gif   6x nearest-neighbour preview of exactly what the gadget will show
  frames/       the individual 64x64 PNGs (QA)
  manifest.json what was made + byte accounting (+ the --fit search table)
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

N = 64  # the gadget avatar grid

BAYER4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]],
                  np.float32) / 16.0

FMT_RGB565, FMT_U8, FMT_I4 = 0, 1, 2
BYTES_PER_FRAME = {FMT_RGB565: N * N * 2, FMT_U8: N * N, FMT_I4: N * N // 2}


def extract(video, fps, secs, start, tmp, alpha=False):
    """Every frame as a PNG at `fps`, capped at `secs` (ffmpeg does the timing).
    alpha=True keeps the source's transparency so a --bg composite can see it."""
    os.makedirs(tmp, exist_ok=True)
    cmd = ["ffmpeg", "-v", "error"]
    if start:
        cmd += ["-ss", str(start)]
    cmd += ["-i", video]
    if secs:
        cmd += ["-t", str(secs)]
    vf = f"fps={fps}" + (",format=rgba" if alpha else "")
    cmd += ["-vf", vf, os.path.join(tmp, "%04d.png")]
    subprocess.run(cmd, check=True)
    return sorted(f for f in os.listdir(tmp) if f.endswith(".png"))


def build_palette(frames, colors):
    """The clip's own palette, quantized to `colors` (median cut via PIL). Built ONCE over
    a strip of every frame: one palette for the whole clip is what keeps static areas static."""
    strip = Image.new("RGB", (N * len(frames), N))
    for i, f in enumerate(frames):
        strip.paste(f, (i * N, 0))
    q = strip.quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.NONE)
    pal = np.array(q.getpalette()[: colors * 3], np.uint8).reshape(-1, 3)
    return pal


def cell_bounds(px):
    """The 64 cell start/end offsets along one axis (same rounding as v1's snap)."""
    return [int(round(i * px / N)) for i in range(N + 1)]


def snap(frame, pal):
    """Nearest-colour map at full res, then majority colour in each source cell -> 64x64."""
    im = np.asarray(frame.convert("RGB"), np.int32)
    h, w, _ = im.shape
    flat = im.reshape(-1, 1, 3)
    idx = ((flat - pal[None].astype(np.int32)) ** 2).sum(2).argmin(1).reshape(h, w)
    ys, xs = cell_bounds(h), cell_bounds(w)
    out = np.zeros((N, N), np.int32)
    for y in range(N):
        for x in range(N):
            cell = idx[ys[y]:ys[y + 1], xs[x]:xs[x + 1]]
            out[y, x] = np.bincount(cell.ravel(), minlength=len(pal)).argmax()
    return pal[out].astype(np.uint8), out


def snap_bayer(frame, pal):
    """Ordered-dither snap: each cell takes its MEAN colour, then picks between the two
    nearest palette entries with the firmware's own 4x4 Bayer matrix. The pattern is a
    function of the CELL POSITION only, so a static area keeps the same pixels every frame
    (the whole point -- error diffusion would not)."""
    im = np.asarray(frame.convert("RGB"), np.float32)
    h, w, _ = im.shape
    ys, xs = cell_bounds(h), cell_bounds(w)
    means = np.empty((N, N, 3), np.float32)
    for y in range(N):
        row = im[ys[y]:ys[y + 1]]
        for x in range(N):
            means[y, x] = row[:, xs[x]:xs[x + 1]].mean(axis=(0, 1))
    d = ((means[:, :, None, :] - pal[None, None, :, :].astype(np.float32)) ** 2).sum(3)
    order = np.argsort(d, axis=2)
    i1 = order[:, :, 0]
    i2 = order[:, :, 1] if d.shape[2] > 1 else i1
    d1 = np.sqrt(np.take_along_axis(d, i1[:, :, None], 2)[:, :, 0])
    d2 = np.sqrt(np.take_along_axis(d, i2[:, :, None], 2)[:, :, 0]) if d.shape[2] > 1 else d1
    t = d1 / (d1 + d2 + 1e-9)                    # 0 = exactly c1, 1 = halfway to c2
    yy, xx = np.mgrid[0:N, 0:N]
    pick2 = t > BAYER4[yy % 4, xx % 4]
    idx = np.where(pick2, i2, i1).astype(np.int32)
    return pal[idx].astype(np.uint8), idx


def fade_mask():
    """True where the round screen KEEPS the pixel (the same rim the firmware's vignette uses)."""
    yy, xx = np.mgrid[0:N, 0:N]
    d = np.sqrt((xx - (N - 1) / 2) ** 2 + (yy - (N - 1) / 2) ** 2)
    kk = np.clip((N / 2 - 1 - d) / 5.0, 0, 1)
    return (kk >= 1) | (BAYER4[yy % 4, xx % 4] < kk)


def circle_fade(img):
    """The round-screen vignette the gadget applies: ordered-dither fade at the rim."""
    img = img.copy()
    img[~fade_mask()] = 0
    return img


def to565(img):
    r = (img[:, :, 0].astype(np.uint16) >> 3) << 11
    g = (img[:, :, 1].astype(np.uint16) >> 2) << 5
    b = img[:, :, 2].astype(np.uint16) >> 3
    return (r | g | b).astype(np.uint16)


def pack_i4(idx):
    """64x64 palette indices -> 32x64 packed bytes. Even-x pixel in the LOW nibble."""
    lo = idx[:, 0::2].astype(np.uint8)
    hi = idx[:, 1::2].astype(np.uint8)
    return (lo | (hi << 4)).astype(np.uint8)


def write_header_rgb565(path, name, frames565, fps, colors):
    up = name.upper()
    with open(path, "w") as f:
        f.write(f"// Generated by video_to_gadget.py from a video clip. Do not edit.\n")
        f.write(f"// {len(frames565)} frames, 64x64 RGB565, {fps} fps, {colors}-colour palette.\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write(f"#define {up}_W 64\n#define {up}_H 64\n")
        f.write(f"#define {up}_COUNT {len(frames565)}\n")
        f.write(f"#define {up}_FPS {fps}\n")
        f.write(f"static const uint16_t {up}_FRAMES[{len(frames565)} * 64 * 64] = {{\n")
        for fr in frames565:
            row = ",".join("0x%04x" % v for v in fr.ravel())
            f.write("  " + row + ",\n")
        f.write("};\n")


def write_header_indexed(path, name, frames_idx, pal565, fps, colors, fmt, dither, source):
    """Indexed header: palette + indices, plus a registry ENTRY macro (bench_videos.h)."""
    up = name.upper()
    kind = "u8" if fmt == FMT_U8 else "i4"
    with open(path, "w") as f:
        f.write(f"// Generated by video_to_gadget.py v2 from {source}. Do not edit.\n")
        f.write(f"// {len(frames_idx)} frames, 64x64, indexed {kind}, {colors} colours, {fps} fps, "
                f"dither={dither}.\n")
        if fmt == FMT_I4:
            f.write("// i4 packing: two pixels/byte, row-major, even-x in the LOW nibble.\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write(f"#define {up}_W 64\n#define {up}_H 64\n")
        f.write(f"#define {up}_COUNT {len(frames_idx)}\n")
        f.write(f"#define {up}_FPS {fps}\n")
        f.write(f"#define {up}_NPAL {len(pal565)}\n")
        f.write(f"#define {up}_FMT {fmt}\n")
        f.write(f"#define {up}_NAME \"{name}\"\n")
        f.write(f"#define {up}_ENTRY {{ {up}_NAME, {up}_COUNT, {up}_FPS, {up}_NPAL, {up}_FMT, "
                f"{up}_PAL, {up}_FRAMES }}\n")
        f.write(f"static const uint16_t {up}_PAL[{len(pal565)}] = {{\n")
        f.write("  " + ", ".join("0x%04x" % int(v) for v in pal565) + ",\n};\n")
        if fmt == FMT_U8:
            f.write(f"static const uint8_t {up}_FRAMES[{len(frames_idx)} * 64 * 64] = {{\n")
            for fr in frames_idx:
                for y in range(N):
                    f.write("  " + ",".join(str(int(v)) for v in fr[y]) + ",\n")
        else:
            packed = [pack_i4(fr) for fr in frames_idx]
            f.write(f"static const uint8_t {up}_FRAMES[{len(frames_idx)} * 32 * 64] = {{\n")
            for pk in packed:
                for y in range(N):
                    f.write("  " + ",".join("0x%02x" % int(v) for v in pk[y]) + ",\n")
        f.write("};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--secs", type=float, default=None)
    ap.add_argument("--start", type=float, default=None)
    ap.add_argument("--colors", type=int, default=24)
    ap.add_argument("--format", choices=["rgb565", "u8", "i4"], default="rgb565")
    ap.add_argument("--dither", choices=["none", "bayer"], default="none")
    ap.add_argument("--bg", default=None, metavar="RRGGBB",
                    help="composite the source over this colour before snapping (transparent GIFs)")
    ap.add_argument("--fit", type=int, default=None, metavar="FLASH_KB",
                    help="best quality that fits: search fps divisors under this data budget")
    ap.add_argument("--name", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--gif", default=None)
    ap.add_argument("--header", default=None)
    a = ap.parse_args()

    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found (brew install ffmpeg)")
    fmt = {"rgb565": FMT_RGB565, "u8": FMT_U8, "i4": FMT_I4}[a.format]
    if fmt == FMT_I4 and a.colors > 16:
        sys.exit("i4 format packs 4-bit indices: --colors must be <= 16")
    if fmt != FMT_RGB565 and a.colors > 32:
        sys.exit("indexed format: keep --colors <= 32 (dither speckle above that at 64x64)")

    name = a.name or os.path.splitext(os.path.basename(a.video))[0]
    name = "".join(ch if ch.isalnum() else "_" for ch in name).strip("_").lower()
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", name)
    os.makedirs(out, exist_ok=True)
    frames_png_dir = os.path.join(out, "frames")
    os.makedirs(frames_png_dir, exist_ok=True)

    search = None
    chosen_fps, chosen_colors = a.fps, a.colors

    with tempfile.TemporaryDirectory() as tmp:
        files = extract(a.video, a.fps, a.secs, a.start, tmp, alpha=bool(a.bg))
        if not files:
            sys.exit("no frames extracted -- is that a video?")
        raw = [Image.open(os.path.join(tmp, f)).convert("RGB") for f in files]
        if a.bg:
            # a transparent source (a GIF) converts to RGB with whatever colour hid under the
            # alpha -- usually white, which would flash the round screen. Composite properly.
            rgb = tuple(int(a.bg[i:i + 2], 16) for i in (0, 2, 4))
            composited = []
            for f, fr in zip(files, raw):
                src = Image.open(os.path.join(tmp, f))
                if src.mode == "RGBA":
                    bg = Image.new("RGB", src.size, rgb)
                    bg.paste(src, mask=src.split()[3])
                    composited.append(bg)
                else:
                    composited.append(fr)
            raw = composited
        print(f"extracted {len(raw)} frames ({a.fps} fps)")

        # --fit: arithmetic search over fps divisors of the extraction rate. The data size
        # does not depend on the pixels, only on (frame count, bytes/frame, palette bytes),
        # so this never needs to render anything to decide.
        if a.fit:
            budget = a.fit * 1024
            cands = []
            for k in (1, 2, 3, 4, 6, 8):
                f_fps = a.fps // k
                if f_fps < 4 or (cands and f_fps == cands[-1][0]):
                    continue
                n_f = len(raw[::k])
                size = n_f * BYTES_PER_FRAME[fmt] + a.colors * 2
                cands.append((f_fps, k, n_f, size))
            search = {"budget_kb": a.fit,
                      "candidates": [{"fps": c[0], "frames": c[2], "data_kb": round(c[3] / 1024)}
                                     for c in cands]}
            pick = next((c for c in cands if c[3] <= budget), None)
            if pick:
                chosen_fps, k, n_f = pick[0], pick[1], pick[2]
                raw = raw[::k]
                print(f"fit: {n_f} frames @ {chosen_fps} fps, {pick[3] / 1024:.0f} KB "
                      f"<= {a.fit} KB")
            else:
                lowfps, k, _, _ = cands[-1]
                per = BYTES_PER_FRAME[fmt]
                n_f = max(1, (budget - a.colors * 2) // per)
                raw = raw[::k][:n_f]
                chosen_fps = lowfps
                search["truncated_to_frames"] = n_f
                print(f"fit: nothing fits; truncated to {n_f} frames @ {lowfps} fps")
            search["chosen"] = {"fps": chosen_fps, "frames": len(raw)}

        pal = build_palette(raw, chosen_colors)
        print(f"palette: {len(pal)} colours ({a.dither} dither)")

        snapper = snap_bayer if a.dither == "bayer" else snap
        # palette's darkest entry stands in for the fade's black (indexed has no RGB black)
        luma = 0.299 * pal[:, 0] + 0.587 * pal[:, 1] + 0.114 * pal[:, 2]
        black_idx = int(np.argmin(luma))
        keep = fade_mask()
        small, idxs, frames565 = [], [], []
        for i, fr in enumerate(raw):
            px, ix = snapper(fr, pal)
            px = circle_fade(px)
            if fmt != FMT_RGB565:
                ix = ix.copy()
                ix[~keep] = black_idx
            idxs.append(ix)
            small.append(px)
            if fmt == FMT_RGB565:
                frames565.append(to565(px))
            Image.fromarray(px).save(os.path.join(frames_png_dir, f"{i:04d}.png"))

        header = a.header or os.path.join(out, f"{name}.h")
        if fmt == FMT_RGB565:
            write_header_rgb565(header, name, frames565, chosen_fps, chosen_colors)
            data_b = len(frames565) * BYTES_PER_FRAME[fmt]
        else:
            pal565 = to565(pal.reshape(1, -1, 3)).ravel()
            write_header_indexed(header, name, idxs, pal565, chosen_fps, chosen_colors,
                                 fmt, a.dither, a.video)
            data_b = len(idxs) * BYTES_PER_FRAME[fmt] + len(pal565) * 2
        print(f"header: {header}  ({data_b / 1024:.0f} KB data, "
              f"{data_b / len(idxs):.0f} B/frame)")

        gif = a.gif or os.path.join(out, "preview.gif")
        dur = int(1000 / chosen_fps)
        ims = [Image.fromarray(f).resize((N * 6, N * 6), Image.NEAREST) for f in small]
        ims[0].save(gif, save_all=True, append_images=ims[1:], duration=dur, loop=0,
                    disposal=2, optimize=False)
        print(f"preview: {gif} ({len(ims)} frames, {dur} ms)")

    # a tiny manifest so the next step knows what it got
    man = {"source": a.video, "frames": len(small), "fps": chosen_fps,
           "colors": len(pal), "format": a.format, "dither": a.dither,
           "bytes_per_frame": BYTES_PER_FRAME[fmt],
           "data_kb": round(data_b / 1024), "flash_kb": round(data_b / 1024),
           "header": header}
    if search:
        man["fit_search"] = search
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(man, f, indent=1)
    print("done")


if __name__ == "__main__":
    main()
