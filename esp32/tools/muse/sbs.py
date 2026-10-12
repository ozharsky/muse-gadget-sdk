#!/usr/bin/env python3
"""Side-by-side of frame N of two animation files (gif or png) for vision QA."""
import sys
from PIL import Image

a, b, out, n = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
ia, ib = Image.open(a), Image.open(b)
ia.seek(n)
ib.seek(n)
ia = ia.convert("RGB")
ib = ib.convert("RGB")
w = Image.new("RGB", (ia.width + ib.width + 8, max(ia.height, ib.height)), (255, 0, 255))
w.paste(ia, (0, 0))
w.paste(ib, (ia.width + 8, 0))
w.save(out)
print("saved", out, w.size)
