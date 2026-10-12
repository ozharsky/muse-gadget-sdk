#!/usr/bin/env python3
"""Bake the A1 face-pose library into shodan_poses.h (stage_b copy of make_shodan_poses.py).

Source: the pack copy at faces_v2/stage_b/shodan/pack (poses_a1/, base_a1.png).
"""
import os
import sys

PACK = os.path.expanduser("~/muse/art-pipeline/faces_v2/stage_b/shodan/pack")
AVATAR = os.path.expanduser("~/muse/art-pipeline/faces_v2/stage_b/shodan")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PACK)

import numpy as np
import shodan_render as sr

# Canonical order = the C enum order emitted into the header. The renderer references
# the P_* names, so this order can never drift from the code.
NAMES = [
    "half", "closed", "left", "right", "up", "down", "wide", "wink", "target",
    "smirk", "squint", "surprised", "pleased", "angry", "evil", "laugh", "sleepy",
    "confused_l", "confused_r", "disgust", "nervous_l", "nervous_r", "proud", "bored",
    "alert", "mouth_1", "mouth_2",
]
EYES_ONLY = {n: ("eyes",) for n in
             ["half", "closed", "left", "right", "up", "down", "wide", "wink", "target"]}
EYES_ONLY.update({"mouth_1": ("mouth",), "mouth_2": ("mouth",)})


def main():
    os.chdir(PACK)
    P = sr.Poses("poses_a1", "base_a1.png", EYES_ONLY)

    idx_all, col_all, table, total = [], [], [], 0
    for i, name in enumerate(NAMES):
        idx, mask = P.get(name)
        ys, xs = np.nonzero(mask)
        off = total
        for y, x in zip(ys, xs):
            idx_all.append(int(y) * 64 + int(x))
            col_all.append(int(idx[y, x]))
        total += len(ys)
        table.append((off, len(ys), name))
        print(f"  {name:12s} {len(ys):4d}  off {off}")
    print(f"total: {len(NAMES)} poses, {total} delta pixels")
    assert len(NAMES) == 27 and total > 4000, "pose set looks wrong"

    def arr(vals, per=18, fmt=str):
        return "\n".join("    " + ", ".join(fmt(v) for v in vals[i:i + per]) + ","
                         for i in range(0, len(vals), per))

    enum_names = ["P_" + n.upper() for n in NAMES]
    out = f"""/* Baked by faces_v2/stage_b/shodan/make_shodan_poses_a1.py from the A1 pose pack
 * (stage_b/shodan/pack/poses_a1). {len(NAMES)} sparse face deltas (pixel index, colour
 * index) against sprite_data.h (A1). The enum below is the single order; regenerate, never edit. */
#ifndef SHODAN_POSES_H
#define SHODAN_POSES_H

#include <stdint.h>

#define POSE_COUNT {len(NAMES)}

typedef struct {{ uint16_t off; uint16_t n; const char *name; }} shodan_pose_t;

enum {{
    {", ".join(enum_names)},
}};

static const uint16_t SHODAN_POSE_IDX[] = {{
{arr(idx_all)}
}};

static const uint8_t SHODAN_POSE_COL[] = {{
{arr(col_all)}
}};

static const shodan_pose_t SHODAN_POSES[POSE_COUNT] = {{
{chr(10).join('    { %d, %d, "%s" },' % (o, n, nm) for o, n, nm in table)}
}};

#endif
"""
    for dest in (os.path.join(AVATAR, "shodan_poses.h"), os.path.join(HERE, "shodan_poses.h")):
        with open(dest, "w") as f:
            f.write(out)
        print("wrote", dest, f"({len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
