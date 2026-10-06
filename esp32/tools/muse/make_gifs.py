#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Render every Muse animation to an animated GIF using the firmware's renderer.

    python3 tools/muse/make_gifs.py [--default | --src FILE] [--gate] [out_dir]   (default: ./gifs)

Draws your own avatar (components/muse/avatar/muse_pixel.c, see AVATAR_RECIPE.md)
when there is one, else the default avatar. Needs a C compiler and Pillow.

anim.c exits non-zero if any animation renders no frames, and --gate makes this
exit 1 if any GIF holds fewer frames than were drawn (PIL merges runs of identical
frames on save, so dead frames vanish silently). For a deeper measure of motion
quality (dead%, mean/median per frame, a reference comparison) see art-pipeline's
qa_motion.py, which dumps the same PPMs.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_SRC = os.path.join(ROOT, "avatar", "muse_pixel.c")
CUSTOM_SRC = os.path.join(ROOT, "components", "muse", "avatar", "muse_pixel.c")
FRAME_MS = 40


def renderer_source(default=False):
    """The muse_pixel.c the firmware build picks: yours if it exists."""
    return DEFAULT_SRC if default or not os.path.exists(CUSTOM_SRC) else CUSTOM_SRC


def render(src, out):
    """Builds tools/muse/anim.c against `src` and writes one GIF per animation to `out`.

    Returns ([(gif path, frames written into it)], [(name, rendered, unique, dead)]).
    The second list is every animation whose GIF holds fewer frames than the renderer
    drew: PIL merges runs of identical frames on save, so dead frames vanish silently.

    Raises subprocess.CalledProcessError (with the compiler's or driver's output) if
    it doesn't build, or if the renderer itself fails.
    """
    from PIL import Image

    os.makedirs(out, exist_ok=True)
    paths = []
    merged = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "muse_anim")
        subprocess.run(
            ["cc", "-O2", "-Wall", "-I", "components/muse", "tools/muse/anim.c", src, "-lm", "-o", exe],
            cwd=ROOT, check=True, capture_output=True, text=True,
        )
        frames_dir = os.path.join(tmp, "frames")
        os.makedirs(frames_dir)
        # anim.c returns non-zero if any animation failed to render (no frames at all).
        run = subprocess.run([exe, frames_dir], capture_output=True, text=True)
        if run.returncode:
            raise subprocess.CalledProcessError(run.returncode, exe, run.stdout, run.stderr)

        for name in sorted(os.listdir(frames_dir)):
            d = os.path.join(frames_dir, name)
            frames = [Image.open(os.path.join(d, f)).convert("RGB") for f in sorted(os.listdir(d))]
            n_rendered = len(frames)   # one PPM per frame the driver drew
            # One shared palette keeps colours from shimmering between frames.
            strip = Image.new("RGB", (frames[0].width, frames[0].height * len(frames)))
            for i, f in enumerate(frames):
                strip.paste(f, (0, i * f.height))
            pal = strip.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
            frames = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
            path = os.path.join(out, name + ".gif")
            frames[0].save(path, save_all=True, append_images=frames[1:], duration=FRAME_MS, loop=0, optimize=False)
            # PIL merges runs of identical frames on save, so a GIF can hold fewer
            # frames than were drawn. Re-open it and check.
            n_read = Image.open(path).n_frames
            paths.append((path, n_read))
            if n_read != n_rendered:
                merged.append((name, n_rendered, n_read, n_rendered - n_read))
    return paths, merged


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out_dir", nargs="?", default=os.path.join(ROOT, "gifs"))
    which = ap.add_mutually_exclusive_group()
    which.add_argument("--default", action="store_true", help="draw the default avatar even if you have your own")
    which.add_argument("--src", help="draw this muse_pixel.c")
    ap.add_argument("--gate", action="store_true",
                    help="exit 1 if any GIF lost frames to identical-frame merging (dead frames)")
    args = ap.parse_args()
    src = os.path.abspath(args.src) if args.src else renderer_source(args.default)
    print(f"renderer: {os.path.relpath(src, ROOT)}")
    try:
        paths, merged = render(src, os.path.abspath(args.out_dir))
    except subprocess.CalledProcessError as e:
        sys.exit((e.stdout or "") + (e.stderr or "") + f"{os.path.relpath(src, ROOT)} doesn't build or render")
    for path, n in paths:
        print(f"{path}  ({n} frames)")
    if merged:
        print(f"\nWARNING: {len(merged)} animation(s) lost frames to identical-frame merging "
              f"(the GIF holds fewer frames than the renderer drew):", file=sys.stderr)
        for name, rendered, unique, dead in merged:
            print(f"  {name}: {rendered} rendered, {unique} unique - {dead} dead frames", file=sys.stderr)
    if args.gate and merged:
        print(f"GATE FAIL: GIFs with dead frames: {', '.join(name for name, *_ in merged)}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
