#!/usr/bin/env python3
"""Assemble pose_dump PPM stills into a labelled grid sheet + a cycling 6x GIF."""
import glob
import os
import sys

from PIL import Image, ImageDraw

d, out_png, out_gif = sys.argv[1], sys.argv[2], sys.argv[3]
files = sorted(glob.glob(os.path.join(d, "*.ppm")))
files.sort(key=lambda p: int(os.path.basename(p).split("_")[0]))
print(len(files), "poses")
TILE = 320
COLS = 7
LBL = 22
imgs, labels = [], []
for f in files:
    im = Image.open(f).convert("RGB")
    base = os.path.basename(f)[:-4]
    labels.append(base.split("_", 1)[1] if "_" in base else base)
    imgs.append(im)
rows = (len(imgs) + COLS - 1) // COLS
W = COLS * (TILE + 6) + 12
H = rows * (TILE + LBL + 6) + 12
sheet = Image.new("RGB", (W, H), (12, 12, 16))
dr = ImageDraw.Draw(sheet)
for i, (im, lb) in enumerate(zip(imgs, labels)):
    r, c = divmod(i, COLS)
    x = 6 + c * (TILE + 6)
    y = 6 + r * (TILE + LBL + 6)
    dr.text((x + 2, y + 4), f"{i + 1}:{lb}", fill=(230, 230, 230))
    sheet.paste(im.resize((TILE, TILE), Image.NEAREST), (x, y + LBL))
sheet.save(out_png)
print("wrote", out_png, sheet.size)

# cycling GIF at 6x with a label bar
frames = []
for im, lb in zip(imgs, labels):
    fr = Image.new("RGB", (384, 384 + 34), (12, 12, 16))
    fr.paste(im.resize((384, 384), Image.NEAREST), (0, 0))
    ImageDraw.Draw(fr).text((6, 388), lb, fill=(235, 235, 235))
    frames.append(fr)
frames[0].save(out_gif, save_all=True, append_images=frames[1:], duration=900, loop=0, optimize=False)
print("wrote", out_gif, os.path.getsize(out_gif) // 1024, "KB")
