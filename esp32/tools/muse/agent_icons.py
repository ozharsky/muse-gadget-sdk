#!/usr/bin/env python3
"""Settings-page agent icons from the portraits: a round 40x40 crop of each face.

    python3 tools/muse/agent_icons.py PORTRAITS_DIR [--preview out.png]

PORTRAITS_DIR is the portrait source art (<agent>/base.png; BEEPBOP's lit face.png). Writes
components/muse/avatar/agent_icons.h: RGB565 then an alpha plane (LV_COLOR_FORMAT_RGB565A8).
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

N = 34
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "components", "muse", "avatar", "agent_icons.h")
# agent, source file, crop box in the render at 1024 px: the whole head, centred, with a margin
# so the round mask never cuts the hair, helmet or antenna
ICONS = (("MUSE", "muse/base.png", (222, 120, 802, 700)),
         ("HERMES", "hermes/base.png", (172, 70, 852, 750)),
         ("BEEPBOP", "beepbop/face.png", (172, 60, 852, 740)))


def icon(path, box):
    im = Image.open(path).convert("RGB").resize((1024, 1024), Image.LANCZOS).crop(box).resize((N * 4, N * 4), Image.LANCZOS)
    mask = Image.new("L", (N * 4, N * 4), 0)
    ImageDraw.Draw(mask).ellipse((2, 2, N * 4 - 3, N * 4 - 3), fill=255)
    return np.asarray(im.resize((N, N), Image.LANCZOS)).astype(np.uint16), np.asarray(mask.resize((N, N), Image.LANCZOS))


def main():
    src = sys.argv[1]
    out = ["// Agent icons for the settings page, cut from the portraits by tools/muse/agent_icons.py.",
           f"// {N}x{N} RGB565A8: the RGB565 plane, then the alpha plane.", "#pragma once", "#include <stdint.h>", "",
           f"#define AGENT_ICON_PX {N}", ""]
    tiles = []
    for name, rel, box in ICONS:
        rgb, a = icon(os.path.join(src, rel), box)
        px = ((rgb[..., 0] >> 3) << 11) | ((rgb[..., 1] >> 2) << 5) | (rgb[..., 2] >> 3)
        data = px.astype("<u2").tobytes() + a.astype(np.uint8).tobytes()
        out.append(f"static const uint8_t AGENT_ICON_{name}[{len(data)}] = {{")
        out += ["    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + "," for i in range(0, len(data), 24)]
        out += ["};", ""]
        tiles.append(Image.fromarray((rgb * (a[..., None] / 255.0)).astype(np.uint8)).resize((N * 4, N * 4), Image.NEAREST))
    open(OUT, "w").write("\n".join(out))
    print("wrote", os.path.relpath(OUT, ROOT))
    if "--preview" in sys.argv:
        sheet = Image.new("RGB", (N * 4 * len(tiles), N * 4))
        for i, t in enumerate(tiles):
            sheet.paste(t, (i * N * 4, 0))
        sheet.save(sys.argv[sys.argv.index("--preview") + 1])


if __name__ == "__main__":
    main()
