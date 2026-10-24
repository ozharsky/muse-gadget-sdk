#!/usr/bin/env python3
"""Review sheets for the portrait faces: every reaction and mode, rendered by the real renderer.

    python3 tools/muse/portrait_review.py OUT_DIR [agent ...]

Builds tools/muse/portrait_sim, renders each agent's pack (portraits/portrait_<agent>.bin) and
writes, per agent: reactions_<agent>_<n>.png (each reaction at five moments) and GIFs of the
modes, speech, petting, idle habits and boredom. Fails if a reaction the pixel faces know has no
portrait recipe, or one doesn't play.
"""
import io
import os
import re
import struct
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AV = os.path.join(ROOT, "components", "muse", "avatar")
AGENTS = {"muse": 0, "hermes": 1, "beepbop": 2}
FPS = 25


def names(path, pattern):
    return re.findall(pattern, open(path).read())


def to_rgb(raw, size):
    a = np.frombuffer(raw, "<u2").reshape(size, size).astype(np.uint32)
    return np.dstack([((a >> 11) & 31) * 255 // 31, ((a >> 5) & 63) * 255 // 63, (a & 31) * 255 // 31]).astype(np.uint8)


def main():
    out = sys.argv[1]
    agents = sys.argv[2:] or list(AGENTS)
    os.makedirs(out, exist_ok=True)
    sim = os.path.join(out, "psim")
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-I", os.path.join(ROOT, "tools/muse/portrait_sim"),
                    "-I", os.path.join(ROOT, "components/muse"), "-I", AV, os.path.join(ROOT, "tools/muse/portrait_sim/sim.c"),
                    os.path.join(AV, "muse_portrait.c"), "-lm", "-o", sim], check=True)

    mine = names(os.path.join(AV, "muse_portrait.c"), r'\{ "([a-z_]+)", [0-9.]+f, V3')
    theirs = set()
    for f in ("muse_pixel.c", "muse_pixel_stone.c", "muse_pixel_golem.c"):
        theirs |= set(names(os.path.join(AV, f), r'\[R_[A-Z_]+\] = \{ "([a-z_]+)"'))
    missing = sorted(theirs - set(mine))
    print(f"{len(mine)} portrait reactions; pixel faces know {len(theirs)}; missing: {missing or 'none'}")

    for agent in agents:
        pack = open(os.path.join(ROOT, "portraits", f"portrait_{agent}.bin"), "rb").read()
        size, jpg_off, jpg_len = struct.unpack_from("<H", pack, 4)[0], *struct.unpack_from("<II", pack, 8)
        base = np.asarray(Image.open(io.BytesIO(pack[jpg_off:jpg_off + jpg_len])).convert("RGB")).astype(np.uint16)
        raw = os.path.join(out, f"{agent}.rgb565")
        ((base[..., 0] >> 3) << 11 | (base[..., 1] >> 2) << 5 | base[..., 2] >> 3).astype("<u2").tofile(raw)
        packf = os.path.join(ROOT, "portraits", f"portrait_{agent}.bin")
        # the sheets zoom on the face: BEEPBOP's glass, or the box all the patches share
        if pack[6] == 1:
            box = struct.unpack_from("<4h", pack, 38)
        else:
            rects = [struct.unpack_from("<4H", pack, 46 + 18 * i + 2) for i in range(pack[7])]
            box = (min(r[0] for r in rects), min(r[1] for r in rects),
                   max(r[0] + r[2] for r in rects), max(r[1] + r[3] for r in rects))
        cx, cy, half = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2, max(box[2] - box[0], box[3] - box[1]) // 2 + 8
        crop = (max(cx - half, 0), max(cy - half, 0), min(cx + half, size), min(cy + half, size))

        def run(script, every):
            r = subprocess.run([sim, packf, raw, str(AGENTS[agent]), script, str(every)], capture_output=True)
            if r.returncode:
                sys.exit(f"{agent} {script}: {r.stderr.decode().strip()}")
            fb = size * size * 2
            return [to_rgb(r.stdout[i:i + fb], size) for i in range(0, len(r.stdout), fb)]

        # each reaction at five moments after it starts (it starts at 0.4 s)
        T, moments = 150, (0.15, 0.5, 1.0, 1.6, 2.4)
        per_page = 12
        for page in range(0, len(mine), per_page):
            batch = mine[page:page + per_page]
            sheet = Image.new("RGB", (110 + T * len(moments), T * len(batch)), (24, 24, 24))
            d = ImageDraw.Draw(sheet)
            for row, name in enumerate(batch):
                frames = run("react:" + name, 1)
                d.text((6, row * T + T // 2 - 6), name, fill=(255, 255, 0))
                for col, m in enumerate(moments):
                    im = Image.fromarray(frames[min(int((0.4 + m) * FPS), len(frames) - 1)]).crop(crop).resize((T, T), Image.LANCZOS)
                    sheet.paste(im, (110 + col * T, row * T))
            sheet.save(os.path.join(out, f"reactions_{agent}_{page // per_page + 1}.png"))
        for script in ("modes", "speak", "pet", "idle", "bored"):
            every = 2 if script != "bored" else 10
            frames = [Image.fromarray(f).resize((192, 192), Image.LANCZOS) for f in run(script, every)]
            frames[0].save(os.path.join(out, f"{script}_{agent}.gif"), save_all=True, append_images=frames[1:],
                           duration=int(1000 * every / FPS), loop=0)
        print(agent, "done")
    if missing:
        sys.exit(1)


if __name__ == "__main__":
    main()
