"""Local visual check of the composed C5 sprite: 6x upscale + eye ASCII + kind stats."""
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, ".")
import golem_char as gc

img = Image.fromarray(gc.PAL[gc.SPR])
img.resize((384, 384), Image.NEAREST).save("golem_final_x6.png")
print("wrote golem_final_x6.png")

RUST = gc.IDX["RUST"]
shell = 0
for y in range(4, 45):
    for x in range(4, 60):
        if gc.SPR[y, x] == RUST:
            shell += 1
print("RUST px on shell (y<45):", shell)
for y in range(20, 40):
    print("%4d " % y + "".join("%X" % v for v in gc.SPR[y, 14:50]))
print("kinds y20-40 (2=lens 1=k1 3=k3):")
for y in range(20, 40):
    print("%4d " % y + "".join(str(v) for v in gc.KIND[y, 14:50]))
