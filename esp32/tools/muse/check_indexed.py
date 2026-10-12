#!/usr/bin/env python3
"""Round-trip check: parse an emitted indexed header, unpack a frame, compare to frames/*.png.

Proves the header parses, the packing order matches what a player must implement, and the
palette/indices agree with the preview frames. Used as the acceptance for pipeline v2.
"""
import re
import sys

import numpy as np
from PIL import Image

hdr_path, frames_dir, frame_no = sys.argv[1], sys.argv[2], int(sys.argv[3])
t = open(hdr_path).read()

up = re.search(r"#define (\w+)_COUNT", t).group(1)
count = int(re.search(r"#define \w+_COUNT (\d+)", t).group(1))
fmt = int(re.search(r"#define \w+_FMT (\d+)", t).group(1))
npal = int(re.search(r"#define \w+_NPAL (\d+)", t).group(1))
print(f"header: {up}  count={count} fmt={fmt} npal={npal}")

pal_txt = re.search(r"_PAL\[\d+\] = \{(.*?)\};", t, re.S).group(1)
pal565 = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{4})", pal_txt)]
frames_txt = re.search(r"_FRAMES\[[^\]]+\] = \{(.*?)\};\s*$", t, re.S).group(1)
vals = [int(x, 16) if x.lower().startswith("0x") else int(x)
        for x in re.findall(r"0x[0-9a-fA-F]{2}|(?<![\w#])\d+", frames_txt)]

pal8 = np.zeros((len(pal565), 3), np.uint8)
for i, v in enumerate(pal565):
    r = ((v >> 11) & 0x1F) << 3 | ((v >> 11) & 0x1F) >> 2
    g = ((v >> 5) & 0x3F) << 2 | ((v >> 5) & 0x3F) >> 4
    b = (v & 0x1F) << 3 | (v & 0x1F) >> 2
    pal8[i] = (r, g, b)

ref = np.asarray(Image.open(f"{frames_dir}/{frame_no:04d}.png").convert("RGB"))
# index the reference by exact palette match (the frames are palette-coloured by construction)
dist = ((ref[:, :, None, :].astype(int) - pal8[None, None, :, :].astype(int)) ** 2).sum(3)
ref_idx = dist.argmin(2)
ref_exact = dist.min(2).max()
print(f"reference frame {frame_no}: max palette-match error {ref_exact} (must be 0)")

if fmt == 2:   # i4 packed, low nibble = even x
    per = 32 * 64
    blob = np.array(vals[frame_no * per:(frame_no + 1) * per], np.uint8).reshape(64, 32)
    lo = blob & 0x0F
    hi = (blob >> 4) & 0x0F
    idx = np.empty((64, 64), np.uint8)
    idx[:, 0::2] = lo
    idx[:, 1::2] = hi
else:          # u8
    per = 64 * 64
    idx = np.array(vals[frame_no * per:(frame_no + 1) * per], np.uint8).reshape(64, 64)

same = int((idx == ref_idx).sum())

# compare inside the round-screen keep region; the faded rim legitimately differs
# (indexed has no RGB black, so the emitter uses the palette's darkest entry there)
Nn = 64
yy, xx = np.mgrid[0:Nn, 0:Nn]
d = np.sqrt((xx - (Nn - 1) / 2) ** 2 + (yy - (Nn - 1) / 2) ** 2)
kk = np.clip((Nn / 2 - 1 - d) / 5.0, 0, 1)
BAY = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0
keep = (kk >= 1) | (BAY[yy % 4, xx % 4] < kk)
same = int((idx[keep] == ref_idx[keep]).sum())
rim_bad = int((idx[~keep] != ref_idx[~keep]).sum())
print(f"unpacked vs preview index map: {same}/{int(keep.sum())} identical inside the bezel "
      f"({100.0 * same / int(keep.sum()):.1f}%), rim mismatches {rim_bad} (expected: the "
      f"fade uses the palette's darkest entry, not RGB black)")
if same != int(keep.sum()):
    d2 = np.argwhere((idx != ref_idx) & keep)[:12]
    print("first mismatches (y,x):", [tuple(p) for p in d2])
    sys.exit(1)
print("ROUND-TRIP OK")
