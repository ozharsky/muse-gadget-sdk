"""Before/after compare sheet for the golem: shipping k4 art vs the C5 stage-B base."""
import os

from PIL import Image, ImageDraw
import sys

S = os.path.expanduser("~/muse/art-pipeline/faces_v2/stage_b")
sys.path.insert(0, os.path.join(S, "golem/pack"))
import golem_char as gc

OLD = os.path.expanduser("~/muse/_archive/2026-10-07/art-pipeline/hermes_gemini6/k4_64.png")
TILE, PAD, LAB_W, HDR = 384, 14, 150, 26

old = Image.open(OLD).convert("RGB").resize((TILE, TILE), Image.NEAREST)
new = Image.fromarray(gc.PAL[gc.SPR]).resize((TILE, TILE), Image.NEAREST)
W = LAB_W + 64 + PAD + TILE + PAD * 2
H = HDR + (64 + PAD + TILE) + PAD
img = Image.new("RGB", (W, H), (14, 14, 18))
dr = ImageDraw.Draw(img)
dr.text((8, 7), "golem: before (shipping jade golem k4) / after (C5 terracotta automaton, stage B)", fill=(240, 240, 240))
for i, (label, tile, base) in enumerate([("before", old, Image.open(OLD).convert("RGB")),
                                          ("after", new, Image.fromarray(gc.PAL[gc.SPR]))]):
    y = HDR + i * (64 + PAD + TILE)
    dr.text((8, y + 40), label, fill=(200, 200, 200))
    img.paste(base.resize((64, 64), Image.NEAREST), (LAB_W, y))
    img.paste(tile, (LAB_W + 64 + PAD, y))
out = os.path.join(S, "sheets", "golem", "compare_golem.png")
img.save(out)
print("wrote", out)
