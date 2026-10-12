#!/usr/bin/env python3
"""ASCII maps: where can a prop pixel actually land, per face?

legend: '.' sky/dark (paintable, no body)  '#' gold body (paintable)
        'o' blocked body (paint rule drops props)  ' ' outside the disc
Each cell = 2x2 px. Column ruler = x/10.
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prop_audit import load_sprite, paintable, AVATAR, B  # noqa: E402


def render(face):
    sp = B.FACES[face]
    spr_n, pal, spr = load_sprite(os.path.join(AVATAR, sp["sprite"]))
    ok = paintable(spr, pal)
    print(f"==== {face} ====")
    print("   " + "".join(str((x // 10) % 10) if x % 10 == 0 else " " for x in range(0, 64, 2)))
    for y in range(0, 64, 2):
        row = []
        for x in range(0, 64, 2):
            cells = []
            for dy in (0, 1):
                for dx in (0, 1):
                    xx, yy = x + dx, y + dy
                    d = math.hypot(xx - 31.5, yy - 31.5)
                    if d > 29.0:
                        cells.append(" ")
                    elif not ok[yy, xx] and spr[yy, xx] != 0:
                        cells.append("o")
                    elif ok[yy, xx]:
                        cells.append("#" if spr[yy, xx] != 0 else ".")
                    else:
                        cells.append("?")  # spr==0 but not darkish -> impossible on these faces
            row.append(cells[0])
        print(f"{y:2} " + "".join(row))


if __name__ == "__main__":
    for f in ("golem", "stone"):
        render(f)
        print()
